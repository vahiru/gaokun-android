// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <mutex>
#include <utility>
#include <vector>
extern "C" {
#include "gaokun3-matcher.h"
}

namespace gaokun3 {
// Enrolment takes kStages frames per finger. Authentications that score at
// least kLearnThreshold add their frame, up to kMaxFrames per finger, so the
// template grows towards the part of the finger used for unlocking. Learning
// from every press of two index fingers over a day filled 45 and 49 frames;
// recognising the last session's presses, 48 frames instead of 32 took 13 and
// 15 of 15 instead of 12 and 13, with no other finger accepted.
constexpr size_t kStages = 16;
constexpr size_t kMaxFrames = 48;
constexpr size_t kMaxFingers = 5;

// One image serves both sensor variants; the board straps pick one at run
// time. The values are stored in the template database.
enum class Sensor : uint32_t { kNone = 0, kFocalTech = 1, kGoodix = 2 };
struct SensorProfile {
    Sensor sensor;
    const char* name;
    uint16_t width, height;   // matcher frame geometry
    size_t captureBytes;      // fpcmd CAPTURE payload
    // Frames are resampled by this before feature extraction. The matcher's
    // sizes and its chance correction were set on Goodix frames, where
    // ridges are about 7.5 samples apart; at another pixel pitch, unrelated
    // frames score higher (Goodix frames resized by 1.15 gave other fingers
    // up to 37.5 instead of 18.5). Templates keep the sensor's own samples.
    float matchScale;
};
// FocalTech FTE7001: AES-encrypted LE16 40 x 196 samples. Its pitch has not
// been measured on real fingers yet, so its frames are not resampled.
inline constexpr SensorProfile kFocalTech{Sensor::kFocalTech, "FocalTech FTE7001",
                                          40, 196, 40 * 196 * 2, 1.0f};
// Goodix GDIX5125: plain LE16 12-bit 36 x 160 samples (GAOKUN3_FPCMD_GOODIX_*).
inline constexpr SensorProfile kGoodix{Sensor::kGoodix, "Goodix GDIX5125",
                                       36, 160, 36 * 160 * 2, 1.0f};
constexpr size_t kMaxCaptureBytes = GAOKUN3_MAX_PIXELS * 2;
// TLMM GPIO61/62 as the ACPI _HID of \_SB.SPBA reads them: 0/1 FocalTech,
// 1/0 Goodix, 0/0 no sensor, 1/1 empty firmware.
const SensorProfile* profileFromStraps(bool gpio61, bool gpio62);

// Enrolment frames first, then learned frames, oldest first.
using Frames = std::vector<Gaokun3Frame>;
// Matcher features of one frame, freed with it; the frame is resampled by
// the matchScale of the sensor with its geometry, or by `scale`.
class FrameFeatures {
  public:
    explicit FrameFeatures(const Gaokun3Frame& frame);
    FrameFeatures(const Gaokun3Frame& frame, float scale);
    ~FrameFeatures();
    FrameFeatures(const FrameFeatures&) = delete;
    FrameFeatures& operator=(const FrameFeatures&) = delete;
    const Gaokun3FeatureSet& get() const { return fs_; }
  private:
    Gaokun3FeatureSet fs_;
};
struct Enrollment {
    int32_t id;
    Frames frames;
    // Features of frames, built by score() or prepare() on first use and kept
    // in step by learn(); one thread at a time may use a given finger.
    mutable std::shared_ptr<std::vector<std::unique_ptr<FrameFeatures>>> features;
};
// Builds the missing feature caches of the fingers, one thread per finger.
void prepare(const std::vector<Enrollment>& fingers);
int64_t randomId();
// FocalTech: decrypt and normalise one capture.
bool decode(const uint8_t* encrypted, Gaokun3Frame* frame);
// Goodix: base is a no-finger frame of the same session (fixed pattern).
bool decodeGoodix(const uint16_t* raw, const uint16_t* base, Gaokun3Frame* frame);
// Goodix: true if a no-finger candidate may replace the current base.
bool goodixBaseUsable(const uint16_t* candidate, const uint16_t* current);
// gaokun3_match_score() of the finger's best frame plus, if that reaches
// kCorroboratedScore, that of the best frame among the next five that is not
// of the same spot as it (scores below kSameSpotScore against it); bestFrame,
// if given, receives the first alone.
float score(const Gaokun3Frame& probe, const Frames& enrolled, float* bestFrame = nullptr);
// The same, with the probe's features extracted once for all fingers and the
// finger's kept in finger.features; its frames are compared on up to
// `threads` threads.
float score(const FrameFeatures& probe, const Enrollment& finger, float* bestFrame = nullptr,
            unsigned threads = 1);
// A finger is accepted at this score. Real Goodix captures of six fingers of
// one person, against templates of 16 to 32 frames of either index finger:
// other fingers reached 35.6 at most (32-frame templates; 33.5 with 16). With
// kCorroboratedScore, eight fingers against templates of up to 48 frames of
// five of them: none reached even 25.
constexpr float kMatchThreshold = 37.0f;
// Learning needs a margin over that: a false accept close to the threshold
// must not become part of the finger.
constexpr float kLearnThreshold = 40.0f;
// A probe this close to one frame of the finger repeats it: frames of one
// press reach it with each other a third of the time, frames of different
// presses 0.3% of the time. Repeats are not learned; two near-identical
// frames would make one chance match count twice in score().
constexpr float kRepeatScore = 60.0f;
// Two frames of a finger scoring this much against each other show the same
// spot. In replays with learning, a left middle finger reached 37.2 against a
// left index grown with learned frames, 20.7 and 16.5 from two of them of one
// spot; with this rule the other fingers stayed at 32.7 and the index fingers
// lost nothing. Frames of different presses reach it about 2% of the time.
constexpr float kSameSpotScore = 25.0f;
// A second frame only corroborates a best frame that is beyond chance. No
// frame of another finger reached 20.7 against a frame of the finger in any
// collected data, but two such chance matches of 16 to 19 at different spots
// add up: against templates of 16 to 32 frames (five fingers, about 100 000
// comparisons) other fingers passed kMatchThreshold up to 15 times for one
// finger, and none with this rule; genuine rates fell by 1 to 2 points.
constexpr float kCorroboratedScore = 22.0f;
// Whether an accepted probe (its finger score, and its score against its best
// frame) is learned: at least kLearnThreshold, not a repeat (kRepeatScore),
// and beyond chance against the enrolment frames alone (kCorroboratedScore).
// Without the last rule one learned false accept would carry the same wrong
// finger's later presses into the template: with one frame of another finger
// planted among a left index's learned frames, that finger's presses were
// accepted 10 of 34 times and 8 more of its frames learned; with the rule none
// was learned, while the index learned 26 instead of 30 frames over 57 of its
// own presses and lost one accept.
bool shouldLearn(const FrameFeatures& probe, const Enrollment& finger, float fingerScore, float bestFrame);
// Appends an authenticated probe, replacing the oldest learned frame when the
// finger is full. Enrolment frames are kept; a feature cache only this finger
// holds is updated with the new frame alone.
void learn(Enrollment& e, const Gaokun3Frame& probe);
// Enrolment takes a frame only if ridges cover at least kEnrollCoverage of it
// (gaokun3_quality_check) and it does not repeat a frame already taken
// (kRepeatScore). In two real enrolments the frames under 0.6 matched 1 of 90
// later presses; presses themselves were under 0.67 in 5% of cases.
constexpr float kEnrollCoverage = 0.6f;
enum class EnrollFrame { kTake, kPartial, kRepeat };
EnrollFrame checkEnrollFrame(const Gaokun3Frame& frame, const Frames& taken);

// Failure count is persisted in Store. Runtime deadlines use a monotonic clock.
// Restarting the HAL at a timed boundary imposes another 30 seconds, never less.
class Lockout {
  public:
    explicit Lockout(int64_t timedDurationMs = 30000)
        : timedDurationMs_(timedDurationMs > 0 ? timedDurationMs : 30000) {}
    int64_t remaining(uint32_t failures, int64_t nowMs);
    void reset();
  private:
    std::mutex mutex_;
    const int64_t timedDurationMs_;
    uint32_t timedCount_ = 0;
    int64_t untilMs_ = 0;
};

// One bounded, atomically replaced database per Android user. No raw struct IO.
// The header records the sensor and geometry; a database for another sensor
// or geometry fails to load.
class Store {
  public:
    Store(std::string path, const SensorProfile& profile)
        : path_(std::move(path)), profile_(&profile) {}
    const SensorProfile& profile() const { return *profile_; }
    bool load();
    bool save();
    int64_t authenticatorId = 0;
    uint32_t failures = 0;
    std::vector<Enrollment> enrollments;
  private:
    std::string path_;
    const SensorProfile* profile_;
};
}  // namespace gaokun3
