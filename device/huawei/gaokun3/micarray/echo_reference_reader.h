/*
 * The capture side of the echo reference (speakerfx/echo_reference.h): what the speakers played
 * since the last call, to be handed to the echo canceller before the capture block it echoes in.
 * See README.md, "回声消除".
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "echo_reference.h"

namespace gaokun::micarray {

class EchoReferenceReader {
  public:
    // Finds speakerfx's ring, loading speakerfx if the HAL has not yet (a stream opening, never
    // the I/O thread); false and why when there is none.
    bool attach(std::string* why);

    // Up to 'maxFrames' samples the speakers played since the last call (mono float, full scale
    // 1.0); 0 when nothing new was played or there is no ring.  The first call after attaching,
    // and a reader that fell more than kMaxBacklog behind, start from (close to) the newest.
    size_t pull(float* out, size_t maxFrames);

    bool attached() const { return mRing != nullptr; }
    uint64_t frames() const { return mFrames; }
    uint64_t skips() const { return mSkips; }

  private:
    static constexpr uint64_t kMaxBacklog = gaokun::echoref::EchoReference::kRate / 2;  // 0.5 s
    static constexpr uint64_t kRestart = gaokun::echoref::EchoReference::kRate / 10;    // 0.1 s

    gaokun::echoref::EchoReference* mRing = nullptr;
    bool mStarted = false;
    uint64_t mCursor = 0;  // next ring frame to hand out
    uint64_t mFrames = 0, mSkips = 0;
};

}  // namespace gaokun::micarray
