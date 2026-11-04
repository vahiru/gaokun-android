// SPDX-License-Identifier: Apache-2.0
// Diagnostic-only raw ADC comparison. No frame/template files or auth callbacks.
#include "../../device/huawei/gaokun3/fingerprint/Core.h"
#include "../../device/huawei/gaokun3/fingerprint/Transport.h"
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <grp.h>
#include <random>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

extern "C" int audit_native_open();
extern "C" int audit_native_apply_raw(const unsigned char*,unsigned char*);
extern "C" void audit_native_close();
extern "C" int audit_engine_open(unsigned,unsigned);
extern "C" int audit_engine_enroll(unsigned,const unsigned char*);
extern "C" int audit_engine_identify(const unsigned char*);
extern "C" int audit_engine_quality(const unsigned char*);
extern "C" void audit_engine_close();

// FocalTech-only audit (40 x 196 encrypted frames).
constexpr size_t kImageBytes=gaokun3::kFocalTech.captureBytes;
constexpr size_t kPixels=size_t(gaokun3::kFocalTech.width)*gaokun3::kFocalTech.height;
using Cipher=std::array<uint8_t,kImageBytes>;
using Plain=std::array<uint8_t,kImageBytes+EVP_MAX_BLOCK_LENGTH>;
static const uint8_t key[]={0xeb,0x4a,0xa0,0x13,0x68,0xd8,0x46,0x47,0x0d,0x2b,0xae,0xe3,0x9d,0x02,0x0f,0xdf};
static const uint8_t iv[]={0xd5,0xaa,0x36,0xa5,0x4b,0x7b,0x18,0x35,0xf2,0x37,0xd3,0xf0,0xc5,0x45,0x49,0xbb};
static bool decrypt(const Cipher& cipher,Plain& plain) {
    auto* ctx=EVP_CIPHER_CTX_new();if (!ctx) return false;
    int bytes=0,tail=0;
    bool ok=EVP_DecryptInit_ex(ctx,EVP_aes_128_cbc(),nullptr,key,iv)==1 &&
            EVP_CIPHER_CTX_set_padding(ctx,0)==1 &&
            EVP_DecryptUpdate(ctx,plain.data(),&bytes,cipher.data(),cipher.size())==1 &&
            EVP_DecryptFinal_ex(ctx,plain.data()+bytes,&tail)==1 &&
            bytes+tail==static_cast<int>(cipher.size());
    EVP_CIPHER_CTX_free(ctx);return ok;
}
static bool inRange(const Plain& plain) {
    for (size_t i=0;i<kPixels;++i)
        if (unsigned(plain[2*i])+(unsigned(plain[2*i+1])<<8)>4096) return false;
    return true;
}
static int selfTest() {
    Plain input{},output{};std::mt19937 random(1);
    for (size_t i=0;i<kPixels;++i) {
        unsigned value=200+(random()&255)*8;input[2*i]=value&255;input[2*i+1]=value>>8;
    }
    std::array<uint8_t,kImageBytes+EVP_MAX_BLOCK_LENGTH> encrypted{};
    auto* ctx=EVP_CIPHER_CTX_new();if (!ctx) return 1;int bytes=0,tail=0;
    bool ok=EVP_EncryptInit_ex(ctx,EVP_aes_128_cbc(),nullptr,key,iv)==1 &&
            EVP_CIPHER_CTX_set_padding(ctx,0)==1 &&
            EVP_EncryptUpdate(ctx,encrypted.data(),&bytes,input.data(),kImageBytes)==1 &&
            EVP_EncryptFinal_ex(ctx,encrypted.data()+bytes,&tail)==1 &&
            bytes+tail==static_cast<int>(kImageBytes);
    EVP_CIPHER_CTX_free(ctx);Cipher cipher{};
    std::copy_n(encrypted.begin(),cipher.size(),cipher.begin());
    ok=ok && decrypt(cipher,output) && inRange(output) &&
       !std::memcmp(input.data(),output.data(),kImageBytes);
    output[0]=255;output[1]=255;ok=ok && !inRange(output);
    if (ok) puts("PASS: synthetic raw AES round-trip and strict ADC range rejection");
    return ok?0:1;
}
static void generic(const char* name,const std::vector<Gaokun3Frame>& frames) {
    Gaokun3FeatureSet reference{};unsigned quality=0,eligible=0,followups=0,matches=0;
    for (const auto& frame : frames) {
        if (!gaokun3_quality_check(&frame,nullptr,nullptr)) continue;
        ++quality;auto probe=gaokun3_feature_extract(&frame);
        if (probe.n_feats>=6) {
            ++eligible;
            if (!reference.n_feats) {reference=probe;probe={nullptr,nullptr,0};}
            else {++followups;matches+=gaokun3_match_score(&probe,&reference)>=12;}
        }
        gaokun3_feature_set_free(&probe);
    }
    gaokun3_feature_set_free(&reference);
    printf("GENERIC %s frames=%zu quality=%u sufficient=%u followups=%u matches=%u\n",
           name,frames.size(),quality,eligible,followups,matches);
}
static void native(unsigned width,unsigned height,const std::vector<Gaokun3Frame>& frames) {
    if (audit_engine_open(width,height)) {puts("NATIVE_INIT_FAILED");return;}
    unsigned quality=0,accepted=0,self=0,followups=0,matches=0;
    for (const auto& frame : frames) {
        if (audit_engine_quality(frame.pixels)) continue;
        ++quality;
        if (!accepted) {
            if (audit_engine_enroll(0,frame.pixels)==0) {
                accepted=1;self=audit_engine_identify(frame.pixels)==0;
            }
        } else {++followups;matches+=audit_engine_identify(frame.pixels)==0;}
    }
    audit_engine_close();
    printf("NATIVE geometry=%ux%u quality_pass=%u reference=%u self=%u followups=%u matches=%u\n",
           width,height,quality,accepted,self,followups,matches);
}
static void pipeline(unsigned mode,const std::vector<Plain>& inputs) {
    // Vendor code is experimental. Each branch runs in a separate child after
    // TA shutdown, so a boundary failure cannot discard the other comparisons.
    const pid_t child=fork();
    if (child<0) {puts("PIPELINE_FORK_FAILED");return;}
    if (child==0) {
        // CPU-only children own no sensor/TA handles; bounding analysis here
        // cannot interrupt a secure call. SIGALRM does not generate a core.
        signal(SIGALRM,SIG_DFL);alarm(20);
        std::vector<Gaokun3Frame> processed;unsigned rejected=0;
        for (const auto& input : inputs) {
            Gaokun3Frame output{};
            output.width=gaokun3::kFocalTech.width;output.height=gaokun3::kFocalTech.height;
            if (audit_native_apply_raw(input.data(),output.pixels)) ++rejected;
            else processed.push_back(output);
        }
        printf("PREPROCESS branch=%u inputs=%zu outputs=%zu rejected=%u\n",
               mode,inputs.size(),processed.size(),rejected);
        if (mode==0) generic("original_preprocess",processed);
        else if (!processed.empty()) native(mode==1?40:196,mode==1?196:40,processed);
        audit_native_close();fflush(stdout);_Exit(0);
    }
    int status=0;
    while (waitpid(child,&status,0)<0) if (errno!=EINTR) {puts("PIPELINE_WAIT_FAILED");return;}
    printf("PIPELINE branch=%u exit=%d signal=%d\n",mode,
           WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    if (argc==2 && !std::strcmp(argv[1],"--synthetic-self-test")) return selfTest();
    if (argc!=2 || std::strcmp(argv[1],"--compare-live-raw")) {
        fputs("Use --synthetic-self-test or --compare-live-raw after stopping HAL.\n",stderr);return 2;
    }
    if (geteuid()==0 && (setgroups(0,nullptr) || setgid(1000) || setuid(1000))) return 1;
    if (geteuid()!=1000 || audit_native_open()) return 1;
    signal(SIGINT,SIG_IGN);signal(SIGTERM,SIG_IGN);signal(SIGHUP,SIG_IGN);
    gaokun3::Transport transport(gaokun3::kFocalTech);bool ok=transport.start();
    std::vector<Cipher> images;unsigned captures=0,duplicates=0,errors=0,framing=0;
    using namespace std::chrono_literals;
    auto deadline=std::chrono::steady_clock::now()+5min;
    const char* stop="/data/local/tmp/gaokun3-fp-template-audit-20261103/stop-raw-contact-audit";
    if (ok) puts("READY_RAW: initialized; frames remain in RAM");else puts("INIT_FAILED");
    while (ok && captures<8 && access(stop,F_OK)!=0 && std::chrono::steady_clock::now()<deadline) {
        uint8_t ready=0;
        if (!transport.status(&ready)) {
            if ((errno==EPROTO || errno==EBADMSG) && ++errors<=5) ++framing;
            else {ok=false;break;}
        } else {
            errors=0;
            if (ready) {
                Cipher image{};
                if (!transport.captureRaw(image.data(),image.size())) {ok=false;break;}
                ++captures;
                if (std::find(images.begin(),images.end(),image)==images.end()) images.push_back(image);
                else ++duplicates;
                printf("PROGRESS_RAW captures=%u unique=%zu\n",captures,images.size());
            }
        }
        std::this_thread::sleep_for(300ms);
    }
    if (!transport.stop()) {
        puts("SHUTDOWN_FAILED: preserving ownership; do not kill or restart HAL");for (;;) pause();
    }
    puts("TA_SHUTDOWN_COMPLETE: analysis is CPU-only");
    std::vector<Gaokun3Frame> baseline;
    std::vector<Plain> inputs;
    unsigned aesErrors=0,rangeErrors=0;
    for (const auto& image : images) {
        Gaokun3Frame original{};
        if (gaokun3::decode(image.data(),&original)) baseline.push_back(original);
        Plain plain{};
        if (!decrypt(image,plain)) {++aesErrors;continue;}
        if (!inRange(plain)) {++rangeErrors;continue;}
        inputs.push_back(plain);
    }
    printf("RAW_SUMMARY captures=%u unique=%zu duplicates=%u framing=%u aes_errors=%u "
           "range_errors=%u adc_frames=%zu io_ok=%u\n",captures,images.size(),duplicates,
           framing,aesErrors,rangeErrors,inputs.size(),unsigned(ok));
    generic("percentile",baseline);
    if (!inputs.empty()) for (unsigned mode=0;mode<3;++mode) pipeline(mode,inputs);
    audit_native_close();
    puts("COMPLETE: no frames or templates persisted");
    return ok?0:1;
}
