// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Core.h"
namespace gaokun3 {
class Transport {
  public:
    ~Transport();
    bool start();
    bool stop();
    bool status(uint8_t* value);
    bool captureEncoded(std::array<uint8_t,kImageBytes>& encrypted);
    bool capture(Gaokun3Frame* frame, bool* usable);
  private:
    int fd_ = -1;
    int gpioFd_ = -1;
    bool loaded_ = false;
    bool poisoned_ = false;
    bool reset();
};
}
