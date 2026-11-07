// SPDX-License-Identifier: Apache-2.0
// Generated images only; no biometric input or device access.
extern "C" {
#include "gaokun3-matcher.h"
}
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>

// The suite runs once per sensor geometry (FocalTech 40 x 196, Goodix 36 x 160).
static int W = 40, H = 196;
// Core accepts a finger at 33, the sum of its two best frames. One view of
// the same scene reaches it alone; unrelated scenes stay under half of it.
constexpr float kAccept = 33, kChance = 16.5f;

// Views are cut from a larger generated scene, so a turned or shifted view
// still has texture everywhere instead of blank corners.
constexpr int kSceneSide = 320;
using Scene = std::array<uint8_t, kSceneSide * kSceneSide>;
static Scene scene(unsigned seed) {
    Scene values{};
    std::mt19937 random(seed);
    for (auto& value : values) value = random() & 255;
    return values;
}
static Gaokun3Frame view(const Scene& values, double degrees, double shiftX = 0, double shiftY = 0) {
    constexpr double pi = 3.14159265358979323846;
    const double c = std::cos(degrees * pi / 180), s = std::sin(degrees * pi / 180);
    Gaokun3Frame image{};
    image.width = W;
    image.height = H;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double dx = x - (W - 1) / 2.0, dy = y - (H - 1) / 2.0;
            const double sx = c * dx + s * dy + 159.5 + shiftX, sy = -s * dx + c * dy + 159.5 + shiftY;
            const int ix = std::floor(sx), iy = std::floor(sy);
            const double fx = sx - ix, fy = sy - iy;
            assert(ix >= 0 && ix + 1 < kSceneSide && iy >= 0 && iy + 1 < kSceneSide);
            const auto* p = values.data() + iy * kSceneSide + ix;
            image.pixels[y * W + x] = std::lround((1 - fy) * ((1 - fx) * p[0] + fx * p[1]) +
                                                  fy * ((1 - fx) * p[kSceneSide] + fx * p[kSceneSide + 1]));
        }
    return image;
}
static Gaokun3FeatureSet features(const Gaokun3Frame& image) {
    assert(gaokun3_quality_check(&image, nullptr, nullptr));
    auto fs = gaokun3_feature_extract(&image);
    assert(fs.n_feats > 0 && fs.ridges && fs.width == W && fs.height == H);
    return fs;
}

static void geometryBounds() {
    // Frames without a supported geometry never pass or yield features.
    for (auto [w, h] : {std::pair{0, 0}, {19, 196}, {41, 196}, {40, 197}, {36, 19}}) {
        Gaokun3Frame f{};
        f.width = w;
        f.height = h;
        for (auto& value : f.pixels) value = std::rand() & 255;
        assert(!gaokun3_quality_check(&f, nullptr, nullptr));
        auto fs = gaokun3_feature_extract(&f);
        assert(fs.n_feats == 0 && !fs.kps && !fs.descs && !fs.ridges);
    }
    // A flat frame has no ridges to measure contact or keypoints on.
    Gaokun3Frame flat{};
    flat.width = 36;
    flat.height = 160;
    auto fs = gaokun3_feature_extract(&flat);
    assert(fs.n_feats == 0);
    gaokun3_feature_set_free(&fs);
    gaokun3_feature_set_free(nullptr);
}

static void suite(int width, int height) {
    W = width;
    H = height;
    constexpr size_t count = 32;
    std::array<Gaokun3FeatureSet, count> reference{};
    std::array<Scene, count> scenes{};
    float sameMin = 100, shiftedMin = 100, turnedMax = 0, unrelatedMax = 0;
    for (size_t i = 0; i < count; ++i) {
        scenes[i] = scene(1000 + i);
        reference[i] = features(view(scenes[i], 0));
    }
    for (size_t i = 0; i < count; ++i) {
        for (double degrees : {-10.0, 10.0}) {
            auto probe = features(view(scenes[i], degrees));
            sameMin = std::fmin(sameMin, gaokun3_match_score(&probe, &reference[i]));
            for (size_t j = 0; j < count; ++j)
                if (j != i) unrelatedMax = std::fmax(unrelatedMax, gaokun3_match_score(&probe, &reference[j]));
            gaokun3_feature_set_free(&probe);
        }
        // Three quarters of the strip in common, slightly turned.
        auto shifted = features(view(scenes[i], 5, 2, H / 4.0));
        shiftedMin = std::fmin(shiftedMin, gaokun3_match_score(&shifted, &reference[i]));
        gaokun3_feature_set_free(&shifted);
        // Outside the rotation a press can have: not aligned.
        auto turned = features(view(scenes[i], 60));
        turnedMax = std::fmax(turnedMax, gaokun3_match_score(&turned, &reference[i]));
        gaokun3_feature_set_free(&turned);
    }
    assert(sameMin >= kAccept && shiftedMin >= kAccept);
    assert(turnedMax < kChance && unrelatedMax < kChance);

    // Missing or mismatched inputs never match.
    Gaokun3FeatureSet empty{};
    assert(gaokun3_match_score(nullptr, &reference[0]) == 0 && gaokun3_match_score(&reference[0], nullptr) == 0);
    assert(gaokun3_match_score(&empty, &reference[0]) == 0 && gaokun3_match_score(&reference[0], &empty) == 0);
    W = width == 40 ? 36 : 40;
    H = width == 40 ? 160 : 196;
    auto other = features(view(scenes[0], 0));
    assert(gaokun3_match_score(&other, &reference[0]) == 0);
    gaokun3_feature_set_free(&other);
    W = width;
    H = height;

    for (auto& fs : reference) gaokun3_feature_set_free(&fs);
    // A regression on generated texture, not a claim about real fingers.
    std::cout << "PASS " << width << "x" << height << ": turned views min " << sameMin
              << ", shifted views min " << shiftedMin << ", 60-degree views max " << turnedMax
              << ", 1984 unrelated comparisons max " << unrelatedMax << '\n';
}

static void resizing() {
    // Vertical stripes 10 samples apart are 7.5 apart after shrinking by 0.75.
    Gaokun3Frame stripes{}, out{};
    stripes.width = 40;
    stripes.height = 196;
    for (int y = 0; y < 196; ++y)
        for (int x = 0; x < 40; ++x)
            stripes.pixels[y * 40 + x] = std::lround(128 + 100 * std::cos(2 * 3.14159265358979323846 * x / 10));
    assert(gaokun3_frame_resize(&stripes, 0.75, &out) && out.width == 30 && out.height == 147);
    std::array<double, 8> crossings{};
    int found = 0;
    for (int x = 0; x + 1 < 30 && found < 8; ++x) {
        const double a = out.pixels[70 * 30 + x] - 127.5, b = out.pixels[70 * 30 + x + 1] - 127.5;
        if ((a < 0) != (b < 0)) crossings[found++] = x + a / (a - b);
    }
    assert(found >= 6 && std::fabs((crossings[found - 1] - crossings[0]) / (found - 1) - 3.75) < 0.15);
    // Scale 1 keeps the samples; results without a valid geometry fail.
    assert(gaokun3_frame_resize(&stripes, 1.0, &out) && !memcmp(&out, &stripes, sizeof(out)));
    assert(!gaokun3_frame_resize(&stripes, 1.1, &out) && !gaokun3_frame_resize(&stripes, 0.45, &out));
    assert(!gaokun3_frame_resize(&stripes, 0, &out) && !gaokun3_frame_resize(&stripes, 0.75, &stripes));
    // Views of one scene still align after both are resampled.
    W = 40;
    H = 196;
    const Scene a = scene(77), b = scene(78);
    auto resized = [](const Gaokun3Frame& f) {
        Gaokun3Frame r{};
        assert(gaokun3_frame_resize(&f, 0.85, &r) && r.width == 34 && r.height == 167);
        auto fs = gaokun3_feature_extract(&r);
        assert(fs.n_feats > 0);
        return fs;
    };
    auto ref = resized(view(a, 0)), same = resized(view(a, 10)), other = resized(view(b, 0));
    const float s = gaokun3_match_score(&same, &ref), o = gaokun3_match_score(&other, &ref);
    assert(s >= kAccept && o < kChance);
    for (auto* fs : {&ref, &same, &other}) gaokun3_feature_set_free(fs);
    std::cout << "PASS resampling: stripes 10 -> 7.5 samples, resized views " << s << ", unrelated " << o << '\n';
}

int main() {
    geometryBounds();
    resizing();
    suite(40, 196);
    suite(36, 160);
}
