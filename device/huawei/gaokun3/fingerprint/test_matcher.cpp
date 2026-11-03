// SPDX-License-Identifier: Apache-2.0
// Generated descriptors and images only; no biometric input or device access.
extern "C" {
#include "gaokun3-matcher.h"
}
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <random>

static void similarityGeometry() {
    // Orthogonal descriptors make correspondence unambiguous, isolating the
    // geometric verifier from feature extraction and interpolation effects.
    constexpr int count = 64;
    std::array<Gaokun3Keypoint,count> a{}, b{};
    float descriptors[count][128]{};
    for (int i=0; i<count; ++i) {
        a[i].x = 4 + (i%4)*8;
        a[i].y = 8 + (i/4)*10;
        descriptors[i][i] = 1;
    }
    Gaokun3FeatureSet fa{a.data(),descriptors,count}, fb{b.data(),descriptors,count};
    // Integer-coordinate rounding stays inside the existing 2.5 px tolerance.
    for (double scale : {0.8,0.9,1.0,1.1,1.2}) {
        for (double angle : {-0.1,0.0,0.1}) {
            for (int i=0; i<count; ++i) {
                b[i].x = std::lround(scale*(std::cos(angle)*a[i].x-std::sin(angle)*a[i].y)+5);
                b[i].y = std::lround(scale*(std::sin(angle)*a[i].x+std::cos(angle)*a[i].y)+3);
            }
            assert(gaokun3_match_score(&fa,&fb) == 100.0f);
        }
    }
    // Transform hypotheses outside the existing scale bounds remain rejected.
    for (int i=0; i<count; ++i) { b[i].x=2*a[i].x; b[i].y=2*a[i].y; }
    assert(gaokun3_match_score(&fa,&fb) == 0.0f);
    // Identical descriptors without consistent geometry must not match.
    std::mt19937 random(7001);
    for (auto& point : b) { point.x=random()%40; point.y=random()%196; }
    assert(gaokun3_match_score(&fa,&fb) < 12.0f);
}

static Gaokun3Frame texture(unsigned seed) {
    Gaokun3Frame f{}; std::mt19937 random(seed);
    for (auto& value : f.pixels) value=random()&255;
    return f;
}

static Gaokun3Frame scaled(const Gaokun3Frame& frame, double scale) {
    Gaokun3Frame out{};
    for (int y=0; y<GAOKUN3_HEIGHT; ++y) for (int x=0; x<GAOKUN3_WIDTH; ++x) {
        const double sx=(x-19.5)/scale+19.5, sy=(y-97.5)/scale+97.5;
        const int ix=std::floor(sx), iy=std::floor(sy);
        const double fx=sx-ix, fy=sy-iy;
        auto& value=out.pixels[y*GAOKUN3_WIDTH+x];
        value=128;
        if (ix>=0 && ix+1<GAOKUN3_WIDTH && iy>=0 && iy+1<GAOKUN3_HEIGHT) {
            const auto* p=frame.pixels+iy*GAOKUN3_WIDTH+ix;
            value=std::lround((1-fy)*((1-fx)*p[0]+fx*p[1])+
                             fy*((1-fx)*p[GAOKUN3_WIDTH]+fx*p[GAOKUN3_WIDTH+1]));
        }
    }
    return out;
}

// A larger generated scene supplies real texture beyond the reference crop.
// Rotating an existing 40 x 196 image with blank padding would confound the
// descriptor test with lost sensor coverage and fail the image quality gate.
constexpr int kSceneSide=320;
using Scene=std::array<uint8_t,kSceneSide*kSceneSide>;
static Scene scene(unsigned seed) {
    Scene values{};std::mt19937 random(seed);
    for (auto& value : values) value=random()&255;
    return values;
}
static Gaokun3Frame sceneView(const Scene& values,double degrees) {
    constexpr double pi=3.14159265358979323846;
    const double c=std::cos(degrees*pi/180),s=std::sin(degrees*pi/180);
    Gaokun3Frame image{};
    for (int y=0;y<GAOKUN3_HEIGHT;++y) for (int x=0;x<GAOKUN3_WIDTH;++x) {
        const double dx=x-19.5,dy=y-97.5;
        const double sx=c*dx+s*dy+159.5,sy=-s*dx+c*dy+159.5;
        const int ix=std::floor(sx),iy=std::floor(sy);
        const double fx=sx-ix,fy=sy-iy;
        assert(ix>=0 && ix+1<kSceneSide && iy>=0 && iy+1<kSceneSide);
        const auto* p=values.data()+iy*kSceneSide+ix;
        image.pixels[y*GAOKUN3_WIDTH+x]=std::lround(
            (1-fy)*((1-fx)*p[0]+fx*p[1])+fy*((1-fx)*p[kSceneSide]+fx*p[kSceneSide+1]));
    }
    return image;
}
static void rotatedSceneRegression() {
    constexpr size_t count=32;
    std::array<Gaokun3FeatureSet,count> reference{},rotated{};
    size_t accepted=0;
    for (size_t i=0;i<count;++i) {
        auto values=scene(1000+i);
        auto base=sceneView(values,0);
        assert(gaokun3_quality_check(&base,nullptr,nullptr));
        reference[i]=gaokun3_feature_extract(&base);
        for (double degrees : {-10.0,10.0}) {
            auto image=sceneView(values,degrees);
            assert(gaokun3_quality_check(&image,nullptr,nullptr));
            auto probe=gaokun3_feature_extract(&image);
            accepted+=gaokun3_match_score(&probe,&reference[i])>=12;
            if (degrees>0) rotated[i]=probe;
            else gaokun3_feature_set_free(&probe);
        }
    }
    // A distribution regression, not a claim about real fingerprints. Narrow
    // crops and unstable extrema can still lose genuine correspondences.
    assert(accepted>=48);
    float maximum=0;
    for (size_t i=0;i<count;++i) for (size_t j=0;j<count;++j) {
        if (i==j) continue;
        for (const auto* probe : {&reference[i],&rotated[i]}) {
            const float value=gaokun3_match_score(probe,&reference[j]);
            maximum=std::fmax(maximum,value);
            assert(value<12);
        }
    }
    for (auto& value : reference) gaokun3_feature_set_free(&value);
    for (auto& value : rotated) gaokun3_feature_set_free(&value);
    std::cout << "PASS: full-coverage rotated scene views " << accepted
              << "/64; 1984 unrelated scene comparisons, maximum=" << maximum << '\n';
}

int main() {
    similarityGeometry();
    rotatedSceneRegression();
    auto original=texture(1);
    auto reference=gaokun3_feature_extract(&original);
    for (double scale : {0.9,1.1}) {
        auto image=scaled(original,scale);
        assert(gaokun3_quality_check(&image,nullptr,nullptr));
        auto probe=gaokun3_feature_extract(&image);
        assert(gaokun3_match_score(&probe,&reference) >= 12.0f);
        gaokun3_feature_set_free(&probe);
    }
    gaokun3_feature_set_free(&reference);

    // A broader, fixed negative cohort catches accidental loss of geometric
    // rejection. This synthetic check cannot establish real-finger FAR/FRR.
    std::array<Gaokun3FeatureSet,32> features{};
    for (size_t i=0; i<features.size(); ++i) {
        auto image=texture(100+i);
        features[i]=gaokun3_feature_extract(&image);
        assert(features[i].n_feats >= 6);
        assert(gaokun3_match_score(&features[i],&features[i]) >= 12.0f);
    }
    float maximum=0;
    for (size_t i=0; i<features.size(); ++i) for (size_t j=0; j<features.size(); ++j) {
        if (i==j) continue;
        float value=gaokun3_match_score(&features[i],&features[j]);
        maximum=std::fmax(maximum,value);
        assert(value < 12.0f);
    }
    for (auto& feature : features) gaokun3_feature_set_free(&feature);
    std::cout << "PASS: similarity geometry, scale bounds, scaled image regression; "
                 "992 unrelated texture comparisons, maximum=" << maximum << '\n';
}
