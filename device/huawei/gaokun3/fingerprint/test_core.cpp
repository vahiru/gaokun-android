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
#include <random>
#include <sys/stat.h>
#include <unistd.h>
using namespace gaokun3;
static Gaokun3Frame synthetic(unsigned seed) {
    Gaokun3Frame f{}; std::mt19937 rng(seed);
    for (auto& p : f.pixels) p = rng() & 255;
    return f;
}
int main() {
    char tmp[] = "/tmp/gk3-fp-test-XXXXXX";
    char* dir = mkdtemp(tmp); assert(dir);
    std::string path = std::string(dir)+"/templates.db";
    Store s(path); assert(s.load()); assert(s.enrollments.empty());
    assert(s.save());
    Enrollment e{}; e.id = 7;
    for (size_t i = 0; i < kStages; ++i) e.frames[i] = synthetic(i+1);
    s.enrollments.push_back(e); s.authenticatorId = randomId(); assert(s.authenticatorId > 0);
    s.failures = 5;
    assert(s.save());
    Store restored(path);
    assert(restored.load()); assert(restored.failures == 5);
    assert(restored.authenticatorId == s.authenticatorId);
    assert(restored.enrollments.size() == 1 && restored.enrollments[0].id == 7);
    assert(!memcmp(restored.enrollments[0].frames.data(),e.frames.data(),sizeof(Frames)));
    struct stat st{}; assert(!stat(path.c_str(),&st)); assert((st.st_mode & 0777) == 0600);
    assert(!chmod(path.c_str(),0644)); assert(!restored.load()); assert(restored.enrollments.empty());
    assert(!chmod(path.c_str(),0600)); assert(restored.load());
    { std::fstream f(path,std::ios::in | std::ios::out | std::ios::binary); f.seekp(50); f.put(0x7f); }
    assert(!restored.load()); assert(restored.enrollments.empty());
    assert(s.save()); assert(!truncate(path.c_str(),100)); assert(!restored.load());
    assert(s.save());
    std::string link = std::string(dir)+"/link";
    assert(!symlink(path.c_str(),link.c_str())); Store symlinked(link); assert(!symlinked.load());
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
    assert(same >= 12 && other < 12);
    Gaokun3Frame flat{}; assert(!gaokun3_quality_check(&flat,nullptr,nullptr));
    auto fs = gaokun3_feature_extract(&flat); assert(fs.n_feats == 0); gaokun3_feature_set_free(&fs);
    // Valid AES ciphertext for a uniform LE16 frame must be rejected after decode.
    static const uint8_t key[] = {0xeb,0x4a,0xa0,0x13,0x68,0xd8,0x46,0x47,0x0d,0x2b,0xae,0xe3,0x9d,0x02,0x0f,0xdf};
    static const uint8_t iv[] = {0xd5,0xaa,0x36,0xa5,0x4b,0x7b,0x18,0x35,0xf2,0x37,0xd3,0xf0,0xc5,0x45,0x49,0xbb};
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
    for (size_t i=0; i<GAOKUN3_PIXELS; ++i) {
        unsigned value = 200 + (255-e.frames[0].pixels[i])*8;
        plain[i*2] = value & 255; plain[i*2+1] = value >> 8;
    }
    encrypt(); assert(decode(cipher.data(),&flat));
    assert(score(flat,e.frames) >= 12); // AES, LE16 layout and inverted scaling agree.
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
    unlink(link.c_str()); unlink(path.c_str()); rmdir(dir);
    std::cout << "PASS: store integrity/permissions/round-trip, matcher, AES decode/invalid frames, persistent failures and lockout deadlines\n";
}
