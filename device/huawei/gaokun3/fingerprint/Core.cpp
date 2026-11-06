// SPDX-License-Identifier: Apache-2.0
#include "Core.h"
#ifdef __ANDROID__
#include <android-base/logging.h>
#endif
#include <algorithm>
#include <cerrno>
#include <cmath>
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

static_assert(kFocalTech.width <= GAOKUN3_MAX_WIDTH && kFocalTech.height <= GAOKUN3_MAX_HEIGHT &&
              kGoodix.width <= GAOKUN3_MAX_WIDTH && kGoodix.height <= GAOKUN3_MAX_HEIGHT,
              "sensor geometry exceeds the matcher frame");

const SensorProfile* profileFromStraps(bool gpio61, bool gpio62) {
    if (!gpio61 && gpio62) return &kFocalTech;
    if (gpio61 && !gpio62) return &kGoodix;
    return nullptr;
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
    constexpr size_t kImageBytes = kFocalTech.captureBytes;
    constexpr size_t kPixels = size_t(kFocalTech.width) * kFocalTech.height;
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
    for (size_t i = 0; i < kPixels; ++i) {
        unsigned v = plain[2*i] | (plain[2*i+1] << 8);
        if (v <= 4096) { ++histogram[v]; ++valid; }
    }
    if (valid < kPixels - 100) return rejectedFrame("sample range");
    unsigned lo = 0, hi = 0, cumulative = 0;
    bool found = false;
    for (unsigned v = 0; v <= 4096; ++v) {
        cumulative += histogram[v];
        if (!found && cumulative > valid/100) { lo = v; found = true; }
        if (cumulative > valid*99/100) { hi = v; break; }
    }
    if (hi <= lo) return rejectedFrame("flat image");
    *frame = Gaokun3Frame{};
    frame->width = kFocalTech.width;
    frame->height = kFocalTech.height;
    for (size_t i = 0; i < kPixels; ++i) {
        int v = plain[2*i] | (plain[2*i+1] << 8);
        frame->pixels[i] = std::clamp((int(hi)-v)*255/int(hi-lo), 0, 255);
    }
    if (!gaokun3_quality_check(frame, nullptr, nullptr)) return rejectedFrame("image quality");
    return true;
}

constexpr size_t kGoodixPixels = size_t(kGoodix.width) * kGoodix.height;
constexpr double kMinRidgeRms = 20;
static bool goodixInRange(const uint16_t* v) {
    return std::none_of(v, v + kGoodixPixels, [](uint16_t s) { return s > 4095; });
}
// base - raw without its 7 x 7 local mean, into hp; returns the RMS. With raw
// null, the base's own pattern.
static double goodixHighPass(const uint16_t* raw, const uint16_t* base, std::vector<double>* hp) {
    constexpr int w = kGoodix.width, h = kGoodix.height, kBox = 3;
    std::vector<double> d(kGoodixPixels);
    hp->resize(kGoodixPixels);
    for (size_t i = 0; i < d.size(); ++i) d[i] = double(base[i]) - (raw ? raw[i] : 0);
    double sum2 = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double sum = 0;
            int n = 0;
            for (int yy = std::max(0, y - kBox); yy <= std::min(h - 1, y + kBox); ++yy)
                for (int xx = std::max(0, x - kBox); xx <= std::min(w - 1, x + kBox); ++xx) {
                    sum += d[size_t(yy) * w + xx];
                    ++n;
                }
            double v = d[size_t(y) * w + x] - sum / n;
            (*hp)[size_t(y) * w + x] = v;
            sum2 += v * v;
        }
    return std::sqrt(sum2 / hp->size());
}
// A finger lowers the Goodix samples (median ~1300 against ~2770 without).
// Subtracting the no-finger frame removes the sensor's fixed pattern, which is
// as strong as the ridges themselves; subtracting the 7 x 7 local mean removes
// the slow pressure and coverage shading and keeps the ridges (period about
// seven samples). A finger's load also weakens the fixed pattern under it, so
// that difference keeps a share of the pattern, the same in every frame,
// which lets unrelated frames correlate; its projection on the base's own
// high-passed pattern (up to half of it) is removed. That needs a base without
// a finger trace: see Transport::status(). The ridges are mapped around 128
// with a gain set by their RMS, bright as on the FocalTech path. Two no-finger
// frames differ by an RMS of about 4 counts and a finger gives about 100: a
// plain percentile stretch would turn that noise into texture that passes the
// quality gate, so frames under kMinRidgeRms are rejected first.
bool decodeGoodix(const uint16_t* raw, const uint16_t* base, Gaokun3Frame* frame) {
    constexpr int w = kGoodix.width, h = kGoodix.height;
    constexpr double kGain = 50;
    if (!base) return rejectedFrame("no base frame");
    if (!goodixInRange(raw) || !goodixInRange(base)) return rejectedFrame("sample range");
    std::vector<double> hp, pattern;
    double rms = goodixHighPass(raw, base, &hp);
    if (rms < kMinRidgeRms) return rejectedFrame("no ridge signal");
    if (goodixHighPass(nullptr, base, &pattern) > 1.0) {
        double dot = 0, norm = 0;
        for (size_t i = 0; i < hp.size(); ++i) {
            dot += hp[i] * pattern[i];
            norm += pattern[i] * pattern[i];
        }
        if (dot > 0) {
            const double share = std::min(0.5, dot / norm);
            double sum2 = 0;
            for (size_t i = 0; i < hp.size(); ++i) {
                hp[i] -= share * pattern[i];
                sum2 += hp[i] * hp[i];
            }
            rms = std::sqrt(sum2 / hp.size());
            if (rms < kMinRidgeRms) return rejectedFrame("no ridge signal");
        }
    }
    *frame = Gaokun3Frame{};
    frame->width = w;
    frame->height = h;
    for (size_t i = 0; i < hp.size(); ++i)
        frame->pixels[i] = uint8_t(std::clamp(128 + hp[i] * kGain / rms + 0.5, 0.0, 255.0));
    if (!gaokun3_quality_check(frame, nullptr, nullptr)) return rejectedFrame("image quality");
    return true;
}

// The status poll says no finger, yet a finger may still be close to the key,
// and the current base may hold one: the kernel's finger detection reads 0
// while a finger stays where it was at INIT. Two frames in the same state
// differ by noise only, which also lets the base follow slow drift. Otherwise
// one of them holds a finger, and a finger lowers the samples, so the
// brighter frame is the one without it.
bool goodixBaseUsable(const uint16_t* candidate, const uint16_t* current) {
    if (!goodixInRange(candidate)) return false;
    if (!current) return true;
    std::vector<double> hp;
    if (goodixHighPass(candidate, current, &hp) < kMinRidgeRms) return true;
    uint64_t c = 0, b = 0;
    for (size_t i = 0; i < kGoodixPixels; ++i) { c += candidate[i]; b += current[i]; }
    return c > b;
}

float score(const Gaokun3Frame& probe, const Frames& enrolled, float* bestFrame) {
    // A probe usually overlaps several frames of its finger, while a chance
    // alignment rarely repeats: the best frame and the best one that shows
    // other skin count together. Two frames of the same spot (learned frames
    // gather where the finger unlocks) would count one chance match twice.
    constexpr size_t kCandidates = 6;
    auto p = gaokun3_feature_extract(&probe);
    std::vector<Gaokun3FeatureSet> frames;
    std::vector<std::pair<float, size_t>> ranked;
    if (p.n_feats > 0) for (const auto& frame : enrolled) {
        // Frames of another geometry never come from this sensor.
        if (frame.width != probe.width || frame.height != probe.height) continue;
        frames.push_back(gaokun3_feature_extract(&frame));
        ranked.emplace_back(gaokun3_match_score(&p, &frames.back()), frames.size() - 1);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const float best = ranked.empty() ? 0 : ranked[0].first;
    float second = 0;
    for (size_t k = 1; k < ranked.size() && k < kCandidates; ++k) {
        if (gaokun3_match_score(&frames[ranked[k].second], &frames[ranked[0].second]) >= kSameSpotScore)
            continue;
        second = ranked[k].first;
        break;
    }
    for (auto& f : frames) gaokun3_feature_set_free(&f);
    gaokun3_feature_set_free(&p);
    if (bestFrame) *bestFrame = best;
    return best + second;
}

EnrollFrame checkEnrollFrame(const Gaokun3Frame& frame, const Frames& taken) {
    float coverage = 0;
    gaokun3_quality_check(&frame, &coverage, nullptr);
    if (coverage < kEnrollCoverage) return EnrollFrame::kPartial;
    float best = 0;
    if (!taken.empty()) score(frame, taken, &best);
    return best >= kRepeatScore ? EnrollFrame::kRepeat : EnrollFrame::kTake;
}

void learn(Enrollment& e, const Gaokun3Frame& probe) {
    if (e.frames.size() < kStages) return;
    if (e.frames.size() >= kMaxFrames) e.frames.erase(e.frames.begin() + kStages);
    e.frames.push_back(probe);
}

static void put(std::vector<uint8_t>& b, uint64_t v, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) b.push_back(v >> (i*8));
}
static uint64_t get(const std::vector<uint8_t>& b, size_t pos, unsigned bytes) {
    uint64_t n = 0;
    for (unsigned i = 0; i < bytes; ++i) n |= uint64_t(b[pos+i]) << (i*8);
    return n;
}
// GK3A v4: "GK3A" 4 0 0 0 | count u32 | width u16 | height u16 | authenticatorId
// u64 | failures u32 | sensor u32 | records (id u32, frame count u16, zero u16,
// that many frames of width * height bytes: kStages enrolment frames, then
// the learned ones) | SHA256. v3 had the same header and records of an id and
// exactly kStages frames. v2 (FocalTech only) stored the pixel count 7840 as
// a u32 at 12 and zero at 28, with v3 records. Both still load; the next save
// writes v4.
constexpr size_t kHeader = 32;
static size_t maxRecordBytes(const SensorProfile& p) {
    return 8 + kMaxFrames * size_t(p.width) * p.height;
}
bool Store::load() {
    enrollments.clear(); authenticatorId = 0; failures = 0;
    int fd = open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT;
    struct stat st{};
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
              (st.st_mode & 0777) == 0600 && st.st_size >= int64_t(kHeader+SHA256_DIGEST_LENGTH) &&
              st.st_size <= int64_t(kHeader+kMaxFingers*maxRecordBytes(*profile_)+SHA256_DIGEST_LENGTH);
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
    const size_t pixels = size_t(profile_->width) * profile_->height;
    unsigned count = get(b, 8, 4);
    const bool geometry = get(b,12,2) == profile_->width && get(b,14,2) == profile_->height &&
                          get(b,28,4) == uint32_t(profile_->sensor);
    bool v4 = !memcmp(b.data(), "GK3A\4\0\0\0", 8) && geometry;
    bool v3 = !memcmp(b.data(), "GK3A\3\0\0\0", 8) && geometry;
    bool v2 = !memcmp(b.data(), "GK3A\2\0\0\0", 8) && profile_->sensor == Sensor::kFocalTech &&
              get(b,12,4) == pixels && !get(b,28,4);
    if ((!v2 && !v3 && !v4) || count > kMaxFingers ||
        get(b,24,4) > 20 || (!count && get(b,24,4))) return false;
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(b.data(), b.size()-sizeof(digest), digest);
    if (memcmp(digest, b.data()+b.size()-sizeof(digest), sizeof(digest))) return false;
    int64_t auth = get(b, 16, 8);
    if ((count && auth <= 0) || (!count && auth)) return false;
    std::set<int32_t> ids;
    const size_t end = b.size()-SHA256_DIGEST_LENGTH;
    pos = kHeader;
    for (unsigned i = 0; i < count; ++i) {
        const size_t head = v4 ? 8 : 4;
        if (end - pos < head) { enrollments.clear(); return false; }
        int32_t id = get(b,pos,4);
        size_t frames = v4 ? get(b,pos+4,2) : kStages;
        if (id <= 0 || !ids.insert(id).second || frames < kStages || frames > kMaxFrames ||
            (v4 && get(b,pos+6,2)) || (end - pos - head) / pixels < frames) {
            enrollments.clear(); return false;
        }
        pos += head;
        Enrollment e{}; e.id = id;
        e.frames.resize(frames);
        for (auto& f : e.frames) {
            f.width = profile_->width;
            f.height = profile_->height;
            memcpy(f.pixels, b.data()+pos, pixels);
            pos += pixels;
        }
        enrollments.push_back(std::move(e));
    }
    if (pos != end) { enrollments.clear(); return false; }
    authenticatorId = auth;
    failures = get(b,24,4);
    return true;
}

bool Store::save() {
    if (enrollments.size() > kMaxFingers) return false;
    std::set<int32_t> ids;
    const size_t pixels = size_t(profile_->width) * profile_->height;
    for (const auto& e : enrollments) {
        if (e.id <= 0 || !ids.insert(e.id).second) return false;
        if (e.frames.size() < kStages || e.frames.size() > kMaxFrames) return false;
        for (const auto& f : e.frames)
            if (f.width != profile_->width || f.height != profile_->height) return false;
    }
    if ((enrollments.empty() && authenticatorId) ||
        (!enrollments.empty() && authenticatorId <= 0) ||
        failures > 20 || (enrollments.empty() && failures)) return false;
    std::vector<uint8_t> b{'G','K','3','A',4,0,0,0};
    put(b,enrollments.size(),4); put(b,profile_->width,2); put(b,profile_->height,2);
    put(b,authenticatorId,8); put(b,failures,4); put(b,uint32_t(profile_->sensor),4);
    for (const auto& e : enrollments) {
        put(b,e.id,4); put(b,e.frames.size(),2); put(b,0,2);
        for (const auto& f : e.frames) b.insert(b.end(),f.pixels,f.pixels+pixels);
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
