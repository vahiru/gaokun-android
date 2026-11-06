// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Core.h"
#include <vector>
namespace gaokun3 {
class Transport {
  public:
    explicit Transport(const SensorProfile& profile) : profile_(&profile) {}
    ~Transport();
    // Reads the board straps once; nullptr if the board has no sensor.
    static const SensorProfile* detect();
    bool start();
    bool stop();
    bool status(uint8_t* value);
    // profile.captureBytes of raw fpcmd CAPTURE data.
    bool captureRaw(uint8_t* data, size_t bytes);
    bool capture(Gaokun3Frame* frame, bool* usable);
  private:
    const SensorProfile* profile_;
    int fd_ = -1;
    int gpioFd_ = -1;
    bool loaded_ = false;
    bool poisoned_ = false;
    // Goodix: no-finger frame of this session and the time its refresh
    // interval counts from, and how many candidates were rejected in a row,
    // since when.
    std::vector<uint16_t> base_;
    int64_t baseMs_ = 0;
    unsigned baseRejects_ = 0;
    int64_t firstRejectMs_ = 0;
    bool reset();
    bool refreshBase();
    void clearBase();
};
}
