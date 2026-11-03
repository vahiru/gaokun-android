// SPDX-License-Identifier: Apache-2.0
// Fixed, read-only on-device audit. Never captures, writes or exports templates.
#include "../../device/huawei/gaokun3/fingerprint/Core.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <grp.h>
#include <random>
#include <unistd.h>

#ifdef GAOKUN3_NATIVE_PREPROCESS_AUDIT
extern "C" int audit_native_open();
extern "C" int audit_native_apply(const unsigned char*, unsigned char*);
extern "C" void audit_native_close();
#endif
#ifdef GAOKUN3_NATIVE_ENGINE_AUDIT
extern "C" int audit_engine_open(unsigned, unsigned);
extern "C" int audit_engine_enroll(unsigned, const unsigned char*);
extern "C" int audit_engine_add_sample(unsigned, unsigned, const unsigned char*);
extern "C" int audit_engine_finish(unsigned);
extern "C" int audit_engine_identify(const unsigned char*);
extern "C" int audit_engine_quality(const unsigned char*);
extern "C" void audit_engine_close();

static Gaokun3Frame nativePattern() {
    int xs[40],ys[40],radii[40],signs[40];
    unsigned state=123456789;
    for (int i=0;i<40;++i) {
        state=state*1664525u+1013904223u;xs[i]=state%40;
        state=state*1664525u+1013904223u;ys[i]=state%196;
        state=state*1664525u+1013904223u;radii[i]=2+state%3;
        state=state*1664525u+1013904223u;signs[i]=(state&1)?1:-1;
    }
    Gaokun3Frame frame{};
    for (int y=0;y<196;++y) for (int x=0;x<40;++x) {
        int dots=128;
        for (int i=0;i<40;++i) {
            int dx=x-xs[i],dy=y-ys[i],square=dx*dx+dy*dy,rr=radii[i]*radii[i];
            if (square<rr) {int w=rr-square;dots+=signs[i]*90*w*w/(rr*rr);}
        }
        frame.pixels[y*40+x]=(110+((x/4)&1)*70+std::clamp(dots,0,255))/2;
    }
    return frame;
}
static int nativeEngineSelfTest() {
    if (audit_engine_open(40,196)) {fputs("Native engine initialization failed.\n",stderr);return 1;}
    auto frame=nativePattern();
    const int enroll=audit_engine_enroll(0,frame.pixels);
    const int same=enroll==0?audit_engine_identify(frame.pixels):-99;
    auto shifted=frame;
    for (int y=0;y<196;++y) for (int x=0;x<40;++x)
        shifted.pixels[y*40+x]=frame.pixels[y*40+std::min(x+2,39)];
    const int second=enroll==0?audit_engine_enroll(1,shifted.pixels):-99;
    const int secondCheck=second==0?audit_engine_identify(shifted.pixels):-99;
    std::mt19937 random(400);
    for (auto& value : frame.pixels) value=random()&255;
    const int negative=enroll==0?audit_engine_identify(frame.pixels):-99;
    const bool ok=enroll==0 && same==0 && second==0 && secondCheck==0 &&
                  (negative==-2 || negative==-3);
    audit_engine_close();
    printf("Synthetic native engine status: enroll=%d same=%d second=%d second_check=%d unrelated=%d\n",
           enroll,same,second,secondCheck,negative);
    if (ok) puts("PASS: isolated original engine enroll/same-image/negative synthetic checks");
    return ok?0:1;
}
static int nativeEngineSummary(const gaokun3::Frames& frames) {
    if (audit_engine_open(40,196)) return 1;
    unsigned quality=0,enrolled=0,self=0,followups=0,holdouts=0;
    for (const auto& frame : frames) {
        if (audit_engine_quality(frame.pixels)) continue;
        ++quality;
        if (!enrolled) {
            enrolled=audit_engine_enroll(0,frame.pixels)==0;
            if (enrolled) self=audit_engine_identify(frame.pixels)==0;
        } else {
            ++followups;holdouts+=audit_engine_identify(frame.pixels)==0;
        }
    }
    audit_engine_close();
    printf("native_quality_pass=%u/16 native_enrolled=%u native_self_checks=%u native_followups=%u "
           "native_holdout_matches=%u\n",quality,enrolled,self,followups,holdouts);
    return 0;
}
#endif

struct Summary {
    unsigned quality = 0, sufficient = 0, selfMatches = 0, mutualPairs = 0, unique = 0;
    int minimum = 0, maximum = 0;
};

static Summary summarize(const gaokun3::Frames& frames) {
    std::array<Gaokun3FeatureSet, gaokun3::kStages> features{};
    Summary result;
    for (size_t i = 0; i < frames.size(); ++i) {
        result.quality += gaokun3_quality_check(&frames[i], nullptr, nullptr);
        features[i] = gaokun3_feature_extract(&frames[i]);
        if (i == 0) result.minimum = features[i].n_feats;
        result.minimum = std::min(result.minimum, features[i].n_feats);
        result.maximum = std::max(result.maximum, features[i].n_feats);
        result.sufficient += features[i].n_feats >= 6;
        result.selfMatches += gaokun3_match_score(&features[i], &features[i]) >= 12;
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) {
            duplicate |= std::memcmp(&frames[i], &frames[j], sizeof(frames[i])) == 0;
            result.mutualPairs += gaokun3_match_score(&features[i], &features[j]) >= 12 &&
                                 gaokun3_match_score(&features[j], &features[i]) >= 12;
        }
        result.unique += !duplicate;
    }
    for (auto& feature : features) gaokun3_feature_set_free(&feature);
    return result;
}

static Gaokun3Frame texture(unsigned seed) {
    Gaokun3Frame frame{};
    std::mt19937 random(seed);
    for (auto& value : frame.pixels) value = random() & 255;
    return frame;
}

static int selfTest() {
    gaokun3::Frames frames{};
    auto blank = summarize(frames);
    if (blank.quality || blank.sufficient || blank.selfMatches || blank.mutualPairs ||
        blank.minimum || blank.maximum || blank.unique != 1) return 1;
    frames.fill(texture(1));
    auto identical = summarize(frames);
    if (identical.quality != 16 || identical.sufficient != 16 || identical.selfMatches != 16 ||
        identical.mutualPairs != 120 || identical.unique != 1) return 1;
    for (size_t i = 0; i < frames.size(); ++i) frames[i] = texture(100 + i);
    auto unrelated = summarize(frames);
    if (unrelated.quality != 16 || unrelated.sufficient != 16 || unrelated.selfMatches != 16 ||
        unrelated.mutualPairs || unrelated.unique != 16) return 1;
    puts("PASS: read-only summary with blank, identical and unrelated synthetic frames");
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--synthetic-self-test")) return selfTest();
    bool nativeMode = false;
    bool engineMode = false;
#ifdef GAOKUN3_NATIVE_ENGINE_AUDIT
    if (argc==2 && !std::strcmp(argv[1],"--synthetic-engine-self-test")) return nativeEngineSelfTest();
    engineMode=argc==2 && (!std::strcmp(argv[1],"--audit-user-0-engine") ||
                          !std::strcmp(argv[1],"--audit-user-0-engine-proxy"));
#endif
#ifdef GAOKUN3_NATIVE_PREPROCESS_AUDIT
    if (argc == 2 && !std::strcmp(argv[1], "--synthetic-native-self-test")) {
        if (audit_native_open()) return 1;
        auto input = texture(1);
        Gaokun3Frame output{};
        const bool ok = audit_native_apply(input.pixels, output.pixels) == 0;
        audit_native_close();
        if (ok) puts("PASS: pinned original preprocess with guarded allocation boundaries");
        return ok ? 0 : 1;
    }
    nativeMode = argc == 2 && (!std::strcmp(argv[1], "--audit-user-0-native-proxy") ||
                              !std::strcmp(argv[1], "--audit-user-0-engine-proxy"));
#endif
    if (argc != 2 || (std::strcmp(argv[1], "--audit-user-0") && !nativeMode && !engineMode)) {
        fputs("Use --synthetic-self-test or --audit-user-0 (fixed read-only device audit).\n", stderr);
        return 2;
    }
    if (geteuid() == 0) {
        if (setgroups(0, nullptr) || setgid(1000) || setuid(1000)) {
            perror("drop to template owner");
            return 1;
        }
    }
    if (geteuid() != 1000) {
        fputs("Requires system UID; refusing to bypass database ownership checks.\n", stderr);
        return 1;
    }
    gaokun3::Store store("/data/vendor_de/0/fpdata/gaokun3-templates.db");
    if (!store.load()) {
        fputs("Database failed read-only validation.\n", stderr);
        return 1;
    }
#ifdef GAOKUN3_NATIVE_PREPROCESS_AUDIT
    if (nativeMode) {
        if (audit_native_open()) { fputs("Pinned native map initialization failed.\n", stderr); return 1; }
        for (auto& entry : store.enrollments) for (auto& frame : entry.frames) {
            Gaokun3Frame output{};
            if (audit_native_apply(frame.pixels, output.pixels)) {
                audit_native_close(); fputs("Native proxy preprocessing failed.\n", stderr); return 1;
            }
            frame = output; // Process-local copy only. Store::save is never called.
        }
        audit_native_close();
        puts("mode=native_proxy (original ADC endpoints unavailable; no database write)");
    }
#endif
    printf("enrollments=%zu frames_per_enrollment=%zu\n", store.enrollments.size(), gaokun3::kStages);
    for (size_t i = 0; i < store.enrollments.size(); ++i) {
#ifdef GAOKUN3_NATIVE_ENGINE_AUDIT
        if (engineMode) {
            if (nativeEngineSummary(store.enrollments[i].frames)) return 1;
            continue;
        }
#endif
        const auto result = summarize(store.enrollments[i].frames);
        // Counts only: no pixels, template IDs, descriptors or match scores.
        printf("entry=%zu quality_pass=%u sufficient_features=%u self_matches=%u "
               "feature_count_min=%d feature_count_max=%d mutually_matching_pairs=%u/120 "
               "unique_frames=%u\n", i, result.quality, result.sufficient, result.selfMatches,
               result.minimum, result.maximum, result.mutualPairs, result.unique);
    }
    return 0;
}
