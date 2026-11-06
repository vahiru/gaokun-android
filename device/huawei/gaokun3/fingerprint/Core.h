// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
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
// template grows towards the part of the finger used for unlocking.
constexpr size_t kStages = 16;
constexpr size_t kMaxFrames = 32;
constexpr size_t kMaxFingers = 5;

// One image serves both sensor variants; the board straps pick one at run
// time. The values are stored in the template database.
enum class Sensor : uint32_t { kNone = 0, kFocalTech = 1, kGoodix = 2 };
struct SensorProfile {
    Sensor sensor;
    const char* name;
    uint16_t width, height;   // matcher frame geometry
    size_t captureBytes;      // fpcmd CAPTURE payload
};
// FocalTech FTE7001: AES-encrypted LE16 40 x 196 samples.
inline constexpr SensorProfile kFocalTech{Sensor::kFocalTech, "FocalTech FTE7001",
                                          40, 196, 40 * 196 * 2};
// Goodix GDIX5125: plain LE16 12-bit 36 x 160 samples (GAOKUN3_FPCMD_GOODIX_*).
inline constexpr SensorProfile kGoodix{Sensor::kGoodix, "Goodix GDIX5125",
                                       36, 160, 36 * 160 * 2};
constexpr size_t kMaxCaptureBytes = GAOKUN3_MAX_PIXELS * 2;
// TLMM GPIO61/62 as the ACPI _HID of \_SB.SPBA reads them: 0/1 FocalTech,
// 1/0 Goodix, 0/0 no sensor, 1/1 empty firmware.
const SensorProfile* profileFromStraps(bool gpio61, bool gpio62);

// Enrolment frames first, then learned frames, oldest first.
using Frames = std::vector<Gaokun3Frame>;
struct Enrollment {
    int32_t id;
    Frames frames;
};
int64_t randomId();
// FocalTech: decrypt and normalise one capture.
bool decode(const uint8_t* encrypted, Gaokun3Frame* frame);
// Goodix: base is a no-finger frame of the same session (fixed pattern).
bool decodeGoodix(const uint16_t* raw, const uint16_t* base, Gaokun3Frame* frame);
// Goodix: true if a no-finger candidate may replace the current base.
bool goodixBaseUsable(const uint16_t* candidate, const uint16_t* current);
// Sum of the two best gaokun3_match_score() values over a finger's frames;
// bestFrame, if given, receives the best of them alone.
float score(const Gaokun3Frame& probe, const Frames& enrolled, float* bestFrame = nullptr);
// A finger is accepted at this score. Real Goodix captures of six fingers of
// one person, against templates of 16 to 32 frames of either index finger:
// other fingers reached 35.6 at most (32-frame templates; 33.5 with 16).
constexpr float kMatchThreshold = 37.0f;
// Learning needs a margin over that: a false accept close to the threshold
// must not become part of the finger.
constexpr float kLearnThreshold = 40.0f;
// A probe this close to one frame of the finger repeats it: frames of one
// press reach it with each other a third of the time, frames of different
// presses 0.3% of the time. Repeats are not learned; two near-identical
// frames would make one chance match count twice in score().
constexpr float kRepeatScore = 60.0f;
// Appends an authenticated probe, replacing the oldest learned frame when the
// finger is full. Enrolment frames are kept.
void learn(Enrollment& e, const Gaokun3Frame& probe);

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
