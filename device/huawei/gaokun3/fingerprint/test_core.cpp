// SPDX-License-Identifier: Apache-2.0
// Synthetic inputs only: never capture or persist a person's fingerprint.
#include "Core.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <random>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
using namespace gaokun3;
constexpr size_t kPixels = size_t(kFocalTech.width) * kFocalTech.height;
static Gaokun3Frame synthetic(unsigned seed, const SensorProfile& p = kFocalTech) {
    Gaokun3Frame f{}; std::mt19937 rng(seed);
    f.width = p.width; f.height = p.height;
    for (size_t i = 0; i < size_t(p.width) * p.height; ++i) f.pixels[i] = rng() & 255;
    return f;
}
static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path,std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),{});
}
static void writeFile(const std::string& path, const std::vector<uint8_t>& b) {
    std::ofstream f(path,std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()),b.size());
}
static bool sameFrames(const Frames& a, const Frames& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (memcmp(&a[i],&b[i],sizeof(Gaokun3Frame))) return false;
    return true;
}
static void resign(std::vector<uint8_t>& b) {
    SHA256(b.data(),b.size()-SHA256_DIGEST_LENGTH,b.data()+b.size()-SHA256_DIGEST_LENGTH);
}
// Goodix-like samples: per-pixel fixed pattern, a finger lowers them by
// ~1400 with ridges of period 7 on top, plus a few counts of noise.
static std::vector<uint16_t> goodixBase(unsigned seed) {
    std::mt19937 rng(seed); std::vector<uint16_t> v(size_t(kGoodix.width)*kGoodix.height);
    for (auto& x : v) x = 2500 + rng() % 500;
    return v;
}
static std::vector<uint16_t> goodixSample(const std::vector<uint16_t>& base, unsigned seed,
                                          double finger) {
    std::mt19937 rng(seed); std::vector<uint16_t> v(base.size());
    for (int y = 0; y < kGoodix.height; ++y) for (int x = 0; x < kGoodix.width; ++x) {
        size_t i = size_t(y) * kGoodix.width + x;
        double ridge = finger ? 150 * std::sin(2 * 3.14159265 * (x * 0.8 + y * 0.6 + seed) / 7) : 0;
        v[i] = uint16_t(std::lround(base[i] - finger + ridge + int(rng() % 9) - 4));
    }
    return v;
}
int main() {
    char tmp[] = "/tmp/gk3-fp-test-XXXXXX";
    char* dir = mkdtemp(tmp); assert(dir);
    std::string path = std::string(dir)+"/templates.db";
    Store s(path,kFocalTech); assert(s.load()); assert(s.enrollments.empty());
    assert(s.save());
    Enrollment e{}; e.id = 7;
    for (size_t i = 0; i < kStages; ++i) e.frames.push_back(synthetic(i+1));
    s.enrollments.push_back(e); s.authenticatorId = randomId(); assert(s.authenticatorId > 0);
    s.failures = 5;
    assert(s.save());
    Store restored(path,kFocalTech);
    assert(restored.load()); assert(restored.failures == 5);
    assert(restored.authenticatorId == s.authenticatorId);
    assert(restored.enrollments.size() == 1 && restored.enrollments[0].id == 7);
    assert(sameFrames(restored.enrollments[0].frames,e.frames));
    struct stat st{}; assert(!stat(path.c_str(),&st)); assert((st.st_mode & 0777) == 0600);
    assert(!chmod(path.c_str(),0644)); assert(!restored.load()); assert(restored.enrollments.empty());
    assert(!chmod(path.c_str(),0600)); assert(restored.load());
    { std::fstream f(path,std::ios::in | std::ios::out | std::ios::binary); f.seekp(50); f.put(0x7f); }
    assert(!restored.load()); assert(restored.enrollments.empty());
    assert(s.save()); assert(!truncate(path.c_str(),100)); assert(!restored.load());
    assert(s.save());
    std::string link = std::string(dir)+"/link";
    assert(!symlink(path.c_str(),link.c_str())); Store symlinked(link,kFocalTech); assert(!symlinked.load());
    s.enrollments.push_back(e); assert(!s.save()); s.enrollments.pop_back();
    s.enrollments[0].id = -1; assert(!s.save()); s.enrollments[0].id = 7;
    // Oversized database, unsupported version, wrong geometry and zero IDs all fail closed.
    for (size_t offset : {size_t(4),size_t(8),size_t(12),size_t(32)}) {
        assert(s.save());
        std::fstream f(path,std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(offset); f.put(0); f.close();
        assert(!restored.load());
    }
    auto same = score(e.frames[0],e.frames);
    auto other = score(synthetic(400),e.frames);
    std::cout << "Synthetic match scores: same=" << same << " other=" << other << '\n';
    assert(same >= kMatchThreshold && other < kMatchThreshold);
    Gaokun3Frame flat{}; flat.width = kFocalTech.width; flat.height = kFocalTech.height;
    assert(!gaokun3_quality_check(&flat,nullptr,nullptr));
    auto fs = gaokun3_feature_extract(&flat); assert(fs.n_feats == 0); gaokun3_feature_set_free(&fs);
    // Valid AES ciphertext for a uniform LE16 frame must be rejected after decode.
    static const uint8_t key[] = {0xeb,0x4a,0xa0,0x13,0x68,0xd8,0x46,0x47,0x0d,0x2b,0xae,0xe3,0x9d,0x02,0x0f,0xdf};
    static const uint8_t iv[] = {0xd5,0xaa,0x36,0xa5,0x4b,0x7b,0x18,0x35,0xf2,0x37,0xd3,0xf0,0xc5,0x45,0x49,0xbb};
    constexpr size_t kImageBytes = kFocalTech.captureBytes;
    std::array<uint8_t,kImageBytes> plain{};
    std::array<uint8_t,kImageBytes+32> cipher{};
    auto encrypt = [&] {
        auto ctx = EVP_CIPHER_CTX_new(); assert(ctx); int n=0,tail=0;
        assert(EVP_EncryptInit_ex(ctx,EVP_aes_128_cbc(),nullptr,key,iv) == 1);
        assert(EVP_CIPHER_CTX_set_padding(ctx,0) == 1);
        assert(EVP_EncryptUpdate(ctx,cipher.data(),&n,plain.data(),plain.size()) == 1);
        assert(EVP_EncryptFinal_ex(ctx,cipher.data()+n,&tail) == 1);
        EVP_CIPHER_CTX_free(ctx); assert(n+tail == kImageBytes);
    };
    encrypt(); assert(!decode(cipher.data(),&flat));
    for (size_t i=0; i<kPixels; ++i) {
        unsigned value = 200 + (255-e.frames[0].pixels[i])*8;
        plain[i*2] = value & 255; plain[i*2+1] = value >> 8;
    }
    encrypt(); assert(decode(cipher.data(),&flat));
    assert(score(flat,e.frames) >= kMatchThreshold); // AES, LE16 layout and inverted scaling agree.
    plain.fill(255); encrypt(); assert(!decode(cipher.data(),&flat));
    Lockout tracker;
    assert(tracker.remaining(5,1000) == 30000);
    assert(tracker.remaining(5,2000) == 29000);
    assert(tracker.remaining(5,31000) == 0);
    assert(tracker.remaining(6,32000) == 0);
    assert(tracker.remaining(10,33000) == 30000);
    Lockout restarted;
    assert(restarted.remaining(10,34000) == 30000);
    assert(restarted.remaining(20,35000) == -1);
    restarted.reset(); assert(restarted.remaining(0,36000) == 0);
    assert(restarted.remaining(5,37000) == 30000);
    s.failures = 0;
    s.enrollments.clear(); s.authenticatorId = 0; assert(s.save()); assert(restored.load());
    assert(restored.enrollments.empty() && restored.authenticatorId == 0);

    // Straps as the ACPI _HID reads them.
    assert(profileFromStraps(false,true) == &kFocalTech);
    assert(profileFromStraps(true,false) == &kGoodix);
    assert(!profileFromStraps(false,false) && !profileFromStraps(true,true));

    // v3 (a record is the id and exactly kStages frames) and v2 (FocalTech,
    // before the sensor field) still load; the next save is v4.
    s.enrollments.push_back(e); s.authenticatorId = randomId(); assert(s.save());
    auto v4 = readFile(path);
    assert(v4[4] == 4 && v4[12] == 40 && v4[14] == 196 && v4[28] == 1);
    assert(v4[36] == kStages && v4[37] == 0 && v4[38] == 0 && v4[39] == 0);
    auto v3 = v4;
    v3[4] = 3; v3.erase(v3.begin()+36,v3.begin()+40); resign(v3);
    writeFile(path,v3); assert(!chmod(path.c_str(),0600));
    assert(restored.load() && restored.enrollments.size() == 1);
    assert(sameFrames(restored.enrollments[0].frames,e.frames));
    auto v2 = v3;
    v2[4] = 2; v2[12] = kPixels & 255; v2[13] = kPixels >> 8; v2[14] = v2[15] = 0;
    v2[28] = 0; resign(v2);
    writeFile(path,v2);
    assert(restored.load() && restored.enrollments.size() == 1);
    assert(sameFrames(restored.enrollments[0].frames,e.frames));
    // A Goodix unit never reads a FocalTech database, v2, v3 or v4.
    Store goodixView(path,kGoodix); assert(!goodixView.load());
    assert(restored.save()); assert(!goodixView.load()); assert(readFile(path)[4] == 4);

    // Learned frames follow the enrolment frames, up to kMaxFrames; the
    // oldest learned one goes first and enrolment frames stay.
    Enrollment grown = e;
    learn(grown,synthetic(500));
    assert(grown.frames.size() == kStages+1 && sameFrames(Frames(grown.frames.begin(),grown.frames.begin()+kStages),e.frames));
    for (unsigned i = 1; i < kMaxFrames-kStages; ++i) learn(grown,synthetic(500+i));
    assert(grown.frames.size() == kMaxFrames);
    learn(grown,synthetic(600));
    assert(grown.frames.size() == kMaxFrames);
    const auto second = synthetic(501), newest = synthetic(600);
    assert(!memcmp(&grown.frames[kStages],&second,sizeof(Gaokun3Frame)));
    assert(!memcmp(&grown.frames.back(),&newest,sizeof(Gaokun3Frame)));
    assert(sameFrames(Frames(grown.frames.begin(),grown.frames.begin()+kStages),e.frames));
    Enrollment unfinished{}; unfinished.id = 9; learn(unfinished,synthetic(1));
    assert(unfinished.frames.empty());
    // They are saved and restored, and a record outside kStages..kMaxFrames
    // frames or with a wrong count fails closed.
    restored.enrollments[0] = grown; assert(restored.save());
    assert(restored.load() && sameFrames(restored.enrollments[0].frames,grown.frames));
    auto tampered = readFile(path);
    tampered[36] = kMaxFrames+1; resign(tampered); writeFile(path,tampered); assert(!restored.load());
    tampered[36] = kStages-1; resign(tampered); writeFile(path,tampered); assert(!restored.load());
    tampered[36] = kMaxFrames-1; resign(tampered); writeFile(path,tampered); assert(!restored.load());
    tampered[36] = kMaxFrames; tampered[38] = 1; resign(tampered); writeFile(path,tampered);
    assert(!restored.load());
    // A failed load leaves nothing behind.
    assert(restored.enrollments.empty() && !restored.authenticatorId);
    restored.enrollments.assign(1,grown); restored.authenticatorId = randomId();
    restored.enrollments[0].frames.resize(kStages-1); assert(!restored.save());
    restored.enrollments[0].frames = grown.frames; restored.enrollments[0].frames.push_back(e.frames[0]);
    assert(!restored.save());
    restored.enrollments[0] = e; assert(restored.save());

    // Goodix database round trip; FocalTech refuses it.
    Store g(path,kGoodix); Enrollment ge{}; ge.id = 3;
    for (size_t i = 0; i < kStages; ++i) ge.frames.push_back(synthetic(50+i,kGoodix));
    g.enrollments.push_back(ge); g.authenticatorId = randomId(); assert(g.save());
    auto gv4 = readFile(path);
    assert(gv4.size() == 32 + 8 + kStages*36*160 + SHA256_DIGEST_LENGTH && gv4[28] == 2);
    Store gRestored(path,kGoodix); assert(gRestored.load());
    assert(sameFrames(gRestored.enrollments[0].frames,ge.frames));
    assert(!restored.load());
    // A frame of the wrong geometry is never saved or matched.
    g.enrollments[0].frames[3] = synthetic(9); assert(!g.save());
    assert(score(synthetic(50,kGoodix),e.frames) == 0);
    assert(score(ge.frames[0],ge.frames) >= kMatchThreshold);

    // Goodix decode: needs a no-finger frame, rejects frames without ridges.
    auto base = goodixBase(1);
    auto finger = goodixSample(base,2,1400), again = goodixSample(base,2,1400);
    auto empty = goodixSample(base,3,0);
    Gaokun3Frame gf{}, gf2{};
    assert(decodeGoodix(finger.data(),base.data(),&gf));
    assert(gf.width == 36 && gf.height == 160);
    assert(decodeGoodix(again.data(),base.data(),&gf2));
    Frames gframes(kStages,gf2); assert(score(gf,gframes) >= kMatchThreshold);
    assert(!decodeGoodix(empty.data(),base.data(),&gf));       // noise only
    assert(!decodeGoodix(finger.data(),nullptr,&gf));           // no base yet
    auto bad = finger; bad[17] = 4096; assert(!decodeGoodix(bad.data(),base.data(),&gf));
    assert(!decodeGoodix(base.data(),base.data(),&gf));        // identical frames
    // Base refresh: another no-finger frame replaces it, a finger does not,
    // even over a small part of the sensor; a base that holds a finger gives
    // way to a no-finger frame; uniform drift is followed either way.
    assert(goodixBaseUsable(empty.data(),base.data()));
    assert(goodixBaseUsable(empty.data(),nullptr));
    assert(!goodixBaseUsable(finger.data(),base.data()));
    assert(!goodixBaseUsable(bad.data(),nullptr));
    auto partial = empty;
    std::copy(finger.begin(),finger.begin()+24*kGoodix.width,partial.begin());
    assert(!goodixBaseUsable(partial.data(),base.data()));
    assert(goodixBaseUsable(empty.data(),finger.data()));
    assert(goodixBaseUsable(empty.data(),partial.data()));
    auto cooler = empty, warmer = empty;
    for (auto& v : cooler) v -= 300;
    for (auto& v : warmer) v += 300;
    assert(goodixBaseUsable(cooler.data(),base.data()));
    assert(goodixBaseUsable(warmer.data(),base.data()));
    unlink(link.c_str()); unlink(path.c_str()); rmdir(dir);
    std::cout << "PASS: store integrity/permissions/round-trip, v2/v3 compatibility, learned frames, sensor-bound databases, "
                 "matcher, AES decode/invalid frames, Goodix decode/base, persistent failures and lockout deadlines\n";
}
