// SPDX-License-Identifier: Apache-2.0
#include "Core.h"
#ifdef __ANDROID__
#include <android-base/logging.h>
#endif
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace gaokun3 {
int64_t Lockout::remaining(uint32_t failures, int64_t nowMs) {
    std::lock_guard lock(mutex_);
    if (!failures) { timedCount_ = 0; untilMs_ = 0; return 0; }
    if (failures >= 20) return -1;
    if (failures % 5 == 0 && timedCount_ != failures) {
        timedCount_ = failures; untilMs_ = nowMs + timedDurationMs_;
    }
    return std::max(int64_t(0), untilMs_ - nowMs);
}
void Lockout::reset() {
    std::lock_guard lock(mutex_);
    timedCount_ = 0; untilMs_ = 0;
}

int64_t randomId() {
    int64_t n = 0;
    do {
        if (RAND_bytes(reinterpret_cast<uint8_t*>(&n), sizeof(n)) != 1) return 0;
        n &= INT64_MAX;
    } while (!n);
    return n;
}

static bool rejectedFrame(const char* stage) {
#ifdef __ANDROID__
    // Validation stage only; never log pixels, ciphertext, templates or scores.
    LOG(INFO) << "Fingerprint frame rejected: " << stage;
#else
    (void)stage;
#endif
    return false;
}

bool decode(const uint8_t* encrypted, Gaokun3Frame* frame) {
    static constexpr uint8_t key[] = {0xeb,0x4a,0xa0,0x13,0x68,0xd8,0x46,0x47,
                                    0x0d,0x2b,0xae,0xe3,0x9d,0x02,0x0f,0xdf};
    static constexpr uint8_t iv[] = {0xd5,0xaa,0x36,0xa5,0x4b,0x7b,0x18,0x35,
                                   0xf2,0x37,0xd3,0xf0,0xc5,0x45,0x49,0xbb};
    std::array<uint8_t, kImageBytes + EVP_MAX_BLOCK_LENGTH> plain{};
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>
        ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int n = 0, tail = 0;
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, key, iv) != 1 ||
        EVP_CIPHER_CTX_set_padding(ctx.get(), 0) != 1 ||
        EVP_DecryptUpdate(ctx.get(), plain.data(), &n, encrypted, kImageBytes) != 1 ||
        EVP_DecryptFinal_ex(ctx.get(), plain.data() + n, &tail) != 1 ||
        n + tail != kImageBytes) return rejectedFrame("AES decode");
    std::array<unsigned, 4097> histogram{};
    unsigned valid = 0;
    for (size_t i = 0; i < GAOKUN3_PIXELS; ++i) {
        unsigned v = plain[2*i] | (plain[2*i+1] << 8);
        if (v <= 4096) { ++histogram[v]; ++valid; }
    }
    if (valid < GAOKUN3_PIXELS - 100) return rejectedFrame("sample range");
    unsigned lo = 0, hi = 0, cumulative = 0;
    bool found = false;
    for (unsigned v = 0; v <= 4096; ++v) {
        cumulative += histogram[v];
        if (!found && cumulative > valid/100) { lo = v; found = true; }
        if (cumulative > valid*99/100) { hi = v; break; }
    }
    if (hi <= lo) return rejectedFrame("flat image");
    for (size_t i = 0; i < GAOKUN3_PIXELS; ++i) {
        int v = plain[2*i] | (plain[2*i+1] << 8);
        frame->pixels[i] = std::clamp((int(hi)-v)*255/int(hi-lo), 0, 255);
    }
    if (!gaokun3_quality_check(frame, nullptr, nullptr)) return rejectedFrame("image quality");
    return true;
}

float score(const Gaokun3Frame& probe, const Frames& enrolled) {
    auto p = gaokun3_feature_extract(&probe);
    float best = 0;
    if (p.n_feats >= 6) for (const auto& frame : enrolled) {
        auto t = gaokun3_feature_extract(&frame);
        if (t.n_feats >= 6) best = std::max(best, gaokun3_match_score(&p, &t));
        gaokun3_feature_set_free(&t);
    }
    gaokun3_feature_set_free(&p);
    return best;
}

static void put(std::vector<uint8_t>& b, uint64_t v, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) b.push_back(v >> (i*8));
}
static uint64_t get(const std::vector<uint8_t>& b, size_t pos, unsigned bytes) {
    uint64_t n = 0;
    for (unsigned i = 0; i < bytes; ++i) n |= uint64_t(b[pos+i]) << (i*8);
    return n;
}
constexpr size_t kHeader = 32, kRecord = 4 + kStages * GAOKUN3_PIXELS;
bool Store::load() {
    enrollments.clear(); authenticatorId = 0; failures = 0;
    int fd = open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT;
    struct stat st{};
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
              (st.st_mode & 0777) == 0600 && st.st_size >= int64_t(kHeader+SHA256_DIGEST_LENGTH) &&
              st.st_size <= int64_t(kHeader+kMaxFingers*kRecord+SHA256_DIGEST_LENGTH);
    std::vector<uint8_t> b(ok ? size_t(st.st_size) : 0);
    size_t pos = 0;
    while (ok && pos < b.size()) {
        ssize_t n = read(fd, b.data()+pos, b.size()-pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        pos += n;
    }
    close(fd);
    if (!ok) return false;
    unsigned count = get(b, 8, 4);
    if (memcmp(b.data(), "GK3A\2\0\0\0", 8) || count > kMaxFingers ||
        get(b,12,4) != GAOKUN3_PIXELS ||
        get(b,24,4) > 20 || get(b,28,4) || (!count && get(b,24,4)) ||
        b.size() != kHeader+count*kRecord+SHA256_DIGEST_LENGTH) return false;
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(b.data(), b.size()-sizeof(digest), digest);
    if (memcmp(digest, b.data()+b.size()-sizeof(digest), sizeof(digest))) return false;
    int64_t auth = get(b, 16, 8);
    if ((count && auth <= 0) || (!count && auth)) return false;
    std::set<int32_t> ids;
    for (unsigned i = 0; i < count; ++i) {
        pos = kHeader+i*kRecord;
        int32_t id = get(b,pos,4);
        if (id <= 0 || !ids.insert(id).second) { enrollments.clear(); return false; }
        Enrollment e{}; e.id = id;
        memcpy(e.frames.data(), b.data()+pos+4, kStages*GAOKUN3_PIXELS);
        enrollments.push_back(std::move(e));
    }
    authenticatorId = auth;
    failures = get(b,24,4);
    return true;
}

bool Store::save() {
    if (enrollments.size() > kMaxFingers) return false;
    std::set<int32_t> ids;
    for (const auto& e : enrollments) if (e.id <= 0 || !ids.insert(e.id).second) return false;
    if ((enrollments.empty() && authenticatorId) ||
        (!enrollments.empty() && authenticatorId <= 0) ||
        failures > 20 || (enrollments.empty() && failures)) return false;
    std::vector<uint8_t> b{'G','K','3','A',2,0,0,0};
    put(b,enrollments.size(),4); put(b,GAOKUN3_PIXELS,4); put(b,authenticatorId,8);
    put(b,failures,4); put(b,0,4);
    for (const auto& e : enrollments) {
        put(b,e.id,4);
        const auto* p = reinterpret_cast<const uint8_t*>(e.frames.data());
        b.insert(b.end(),p,p+kStages*GAOKUN3_PIXELS);
    }
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(b.data(),b.size(),digest); b.insert(b.end(),digest,digest+sizeof(digest));
    std::string dir = path_.substr(0,path_.find_last_of('/'));
    int dfd = open(dir.c_str(),O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) return false;
    // A stale temp file from power loss is disposable; never follow a symlink.
    std::string tmp = path_ + ".tmp";
    unlink(tmp.c_str());
    int fd = open(tmp.c_str(),O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,0600);
    bool ok = fd >= 0;
    size_t pos = 0;
    while (ok && pos < b.size()) {
        ssize_t n = write(fd,b.data()+pos,b.size()-pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        pos += n;
    }
    if (ok) ok = fsync(fd) == 0;
    if (fd >= 0 && close(fd)) ok = false;
    if (ok) ok = rename(tmp.c_str(),path_.c_str()) == 0;
    if (ok) ok = fsync(dfd) == 0;
    close(dfd);
    if (!ok) unlink(tmp.c_str());
    return ok;
}
}  // namespace gaokun3
