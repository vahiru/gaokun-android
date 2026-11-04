// SPDX-License-Identifier: Apache-2.0
// Explicit device diagnostic: frames/features stay in RAM; no database writes.
#include "../../device/huawei/gaokun3/fingerprint/Core.h"
#include "../../device/huawei/gaokun3/fingerprint/Transport.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <grp.h>
#include <random>
#include <thread>
#include <unistd.h>

class ContactAudit {
 public:
    explicit ContactAudit(const std::vector<gaokun3::Enrollment>& entries) {
        for (const auto& entry : entries) for (const auto& frame : entry.frames)
            gallery_.push_back(gaokun3_feature_extract(&frame));
    }
    ContactAudit(const ContactAudit&) = delete;
    ContactAudit& operator=(const ContactAudit&) = delete;
    ~ContactAudit() {
        for (auto& feature : gallery_) gaokun3_feature_set_free(&feature);
        gaokun3_feature_set_free(&reference_);
    }
    void observe(const Gaokun3Frame& frame) {
        ++usable;
        for (const auto& previous : seen_) if (!std::memcmp(&frame,&previous,sizeof(frame))) {
            ++duplicates;return;
        }
        seen_.push_back(frame);
        auto probe=gaokun3_feature_extract(&frame);
        minimum=std::min(minimum,probe.n_feats);maximum=std::max(maximum,probe.n_feats);
        if (probe.n_feats>=6) {
            ++sufficient;
            bool matched=false;
            for (const auto& entry : gallery_)
                if (gaokun3_match_score(&probe,&entry)>=12) {matched=true;break;}
            galleryMatches+=matched;
            if (reference_.n_feats) {
                ++followups;
                referenceMatches+=gaokun3_match_score(&probe,&reference_)>=12;
            } else {
                reference_=probe;probe={nullptr,nullptr,0};
            }
        }
        gaokun3_feature_set_free(&probe);
    }
    unsigned usable=0,duplicates=0,sufficient=0,galleryMatches=0,followups=0,referenceMatches=0;
    int minimum=1000000,maximum=0;
 private:
    std::vector<Gaokun3FeatureSet> gallery_;
    Gaokun3FeatureSet reference_{};
    std::vector<Gaokun3Frame> seen_;
};

static Gaokun3Frame texture(unsigned seed) {
    Gaokun3Frame frame{};std::mt19937 random(seed);
    frame.width=gaokun3::kFocalTech.width;frame.height=gaokun3::kFocalTech.height;
    for (auto& value : frame.pixels) value=random()&255;
    return frame;
}
static int selfTest() {
    gaokun3::Enrollment entry{};entry.id=1;entry.frames.fill(texture(1));
    ContactAudit audit({entry});
    audit.observe(entry.frames[0]);audit.observe(entry.frames[0]);audit.observe(texture(400));
    if (audit.usable!=3 || audit.duplicates!=1 || audit.sufficient!=2 ||
        audit.galleryMatches!=1 || audit.followups!=1 || audit.referenceMatches) return 1;
    puts("PASS: synthetic duplicate, unrelated probe and gallery classification");return 0;
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    if (argc==2 && !std::strcmp(argv[1],"--synthetic-self-test")) return selfTest();
    if (argc!=2 || std::strcmp(argv[1],"--compare-live-user-0")) {
        fputs("Use --synthetic-self-test or --compare-live-user-0 after stopping HAL.\n",stderr);return 2;
    }
    if (geteuid()==0 && (setgroups(0,nullptr) || setgid(1000) || setuid(1000))) {
        perror("drop to system UID");return 1;
    }
    if (geteuid()!=1000) return 1;
    const gaokun3::SensorProfile* profile=gaokun3::Transport::detect();
    if (!profile) {puts("NO_SENSOR: board straps report none");return 1;}
    gaokun3::Store store("/data/vendor_de/0/fpdata/gaokun3-templates.db",*profile);
    if (!store.load() || store.enrollments.empty()) {
        fputs("Read-only template validation failed.\n",stderr);return 1;
    }
    ContactAudit audit(store.enrollments);
    // Never terminate an in-flight secure call or reset the sensor on timeout.
    signal(SIGINT,SIG_IGN);signal(SIGTERM,SIG_IGN);signal(SIGHUP,SIG_IGN);
    gaokun3::Transport transport(*profile);
    bool ok=transport.start();
    unsigned framingErrors=0,consecutiveErrors=0,qualityRejected=0,captures=0,polls=0;
    using namespace std::chrono_literals;
    const char* stopPath="/data/local/tmp/gaokun3-fp-template-audit-20261103/stop-contact-audit";
    const auto deadline=std::chrono::steady_clock::now()+5min;
    if (ok) puts("READY: initialized; touch one enrolled finger and hold briefly");
    else puts("INIT_FAILED: no capture attempted");
    while (ok && captures<8 && access(stopPath,F_OK)!=0 &&
           std::chrono::steady_clock::now()<deadline) {
        uint8_t ready=0;++polls;
        if (!transport.status(&ready)) {
            if ((errno==EPROTO || errno==EBADMSG) && ++consecutiveErrors<=5) ++framingErrors;
            else {ok=false;puts("STATUS_FAILED");break;}
        } else {
            consecutiveErrors=0;
            if (ready) {
                Gaokun3Frame frame{};bool usable=false;
                if (!transport.capture(&frame,&usable)) {ok=false;puts("CAPTURE_FAILED");break;}
                ++captures;
                if (usable) audit.observe(frame);else ++qualityRejected;
                printf("PROGRESS: captures=%u usable=%u\n",captures,audit.usable);
            }
        }
        std::this_thread::sleep_for(300ms);
    }
    if (!transport.stop()) {
        puts("SHUTDOWN_FAILED: retaining ownership; do not kill or restart HAL");
        for (;;) pause();
    }
    printf("SUMMARY: polls=%u captures=%u quality_rejected=%u usable=%u duplicates=%u "
           "sufficient_features=%u feature_count_min=%d feature_count_max=%d "
           "followups=%u matched_first_fresh=%u matched_gallery=%u framing_errors=%u io_ok=%u\n",
           polls,captures,qualityRejected,audit.usable,audit.duplicates,audit.sufficient,
           audit.usable?audit.minimum:0,audit.maximum,audit.followups,audit.referenceMatches,
           audit.galleryMatches,framingErrors,unsigned(ok));
    puts("TA shutdown complete; no images or templates saved");
    return ok?0:1;
}
