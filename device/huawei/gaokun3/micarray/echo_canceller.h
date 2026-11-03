/*
 * WebRTC's AEC3 (the AudioProcessing module with only the echo canceller on) for one mono call
 * stream at 48 kHz, in hops of 480 samples -- the 10 ms frames it works in.  See README.md, "回声消除".
 *
 * Built only with GAOKUN_MICARRAY_AEC (the ROM links AOSP's webrtc_audio_processing); without it
 * create() says why and returns nullptr, and calls have no echo cancellation.
 */
#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace gaokun::micarray {

class EchoCanceller {
  public:
    // AudioProcessing delays what it processes by its band-split filters: 430 samples at 48 kHz
    // (an impulse through it, measured).
    static constexpr size_t kLatencyFrames = 430;
    static constexpr size_t kFrame = 480;

    static std::unique_ptr<EchoCanceller> create(std::string* why);
    virtual ~EchoCanceller() = default;

    // What the speakers played, kFrame samples, full scale 1.0.  Before the capture it echoes in.
    virtual void render(const float* frame) = 0;
    // kFrame capture samples in place, full scale 1.0.
    virtual void process(float* frame) = 0;
    // AEC3's echo path delay estimate in ms, or -1 while it has none.
    virtual int delayMs() = 0;
};

}  // namespace gaokun::micarray
