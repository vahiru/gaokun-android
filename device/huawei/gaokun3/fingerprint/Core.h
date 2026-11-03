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
constexpr size_t kStages = 16;
constexpr size_t kMaxFingers = 5;
constexpr size_t kImageBytes = GAOKUN3_PIXELS * 2;
using Frames = std::array<Gaokun3Frame, kStages>;
struct Enrollment {
    int32_t id;
    Frames frames;
};
int64_t randomId();
bool decode(const uint8_t* encrypted, Gaokun3Frame* frame);
float score(const Gaokun3Frame& probe, const Frames& enrolled);

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
class Store {
  public:
    explicit Store(std::string path) : path_(std::move(path)) {}
    bool load();
    bool save();
    int64_t authenticatorId = 0;
    uint32_t failures = 0;
    std::vector<Enrollment> enrollments;
  private:
    std::string path_;
};
}  // namespace gaokun3
