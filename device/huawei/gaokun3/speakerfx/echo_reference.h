/*
 * echo_reference.h -- what the speakers play, from the playback hook (speakerfx) to the capture
 * hook (micarray), both inside the audio HAL process: the far-end reference of the capture hook's
 * echo canceller (micarray/README.md).
 *
 * speakerfx owns one ring for the life of the process and writes every block it hands back to the
 * HAL, after all of its processing -- the samples ALSA gets for the built-in speakers -- as mono
 * float.  It never waits for the reader: a reader more than kFrames behind has lost data and must
 * notice it from 'written'.  The capture hook finds the ring through
 *
 *   extern "C" EchoReference *playback_hook_echo_reference(void);
 *
 * which it looks up when a call stream opens, by dlopen of the same path the HAL loads speakerfx
 * from (so the same instance, whichever of the two loads it first), keeping the handle.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gaokun::echoref {

struct EchoReference {
    static constexpr uint32_t kRate = 48000;
    static constexpr uint32_t kFrames = 1u << 16;  // 1.37 s

    // Frames ever written; stored (release) after the samples.  The frame with index n lives at
    // mono[n % kFrames].
    std::atomic<uint64_t> written{0};
    // CLOCK_MONOTONIC (ns) of the last write: a reader can tell a stopped speaker stream from a
    // late one.
    std::atomic<int64_t> lastWriteNs{0};
    float mono[kFrames] = {};
};

}  // namespace gaokun::echoref
