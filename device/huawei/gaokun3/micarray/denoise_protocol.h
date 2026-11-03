/*
 * Wire protocol between the capture hook (micarray.cpp) and an optional noise suppression
 * service: DeepFilterNet3, 48 kHz, in hops of 480 frames.  The service is gk3denoised (denoise/);
 * when it is not there or does not answer, the hook runs without it.
 *
 * One SOCK_SEQPACKET connection per stream to kSocketPath.  The client sends Hello, the service
 * answers HelloReply; from then on every message is a BlockHeader followed by 'hops' hops of
 * interleaved S16 frames (hops * kHop * channels samples), and the service answers each with the
 * same number of hops, processed.  Each channel is filtered independently and keeps its state for
 * the whole connection.  Output hop n of a connection is input hop n - kLatencyHops, filtered:
 * the first kLatencyHops output hops are the filter starting up.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace gaokun::micarray::denoise {

constexpr char kSocketPath[] = "/dev/socket/gk3denoise";
constexpr uint32_t kMagic = 0x4e444b47;  // "GKDN"
constexpr uint16_t kVersion = 1;
constexpr uint32_t kRate = 48000;
constexpr uint32_t kHop = 480;
constexpr uint32_t kLatencyHops = 3;  // 1440 frames, 30 ms
constexpr uint16_t kMaxChannels = 2;
constexpr uint16_t kMaxHops = 16;     // per message

enum Status : int16_t {
    kOk = 0,
    kBadRequest = -1,  // wrong magic, version, format or size
    kUnavailable = -2,  // the network cannot run (no NPU, model missing, ...)
    kFailed = -3,       // a block failed; the connection is closed after this answer
};

struct Hello {
    uint32_t magic;
    uint16_t version;
    uint16_t channels;  // 1 .. kMaxChannels
    uint32_t rate;      // kRate
    uint32_t hop;       // kHop
};

struct HelloReply {
    uint32_t magic;
    uint16_t version;
    int16_t status;
    uint32_t latencyHops;  // kLatencyHops
    uint16_t maxHops;      // kMaxHops or fewer
    uint16_t reserved;
};

struct BlockHeader {
    uint32_t seq;     // the client counts messages from 0; the answer repeats it
    uint16_t hops;    // 1 .. maxHops
    int16_t status;   // requests: 0; answers: Status
};

constexpr size_t kMaxMessageBytes = sizeof(BlockHeader) + size_t{kMaxHops} * kHop * kMaxChannels * 2;

}  // namespace gaokun::micarray::denoise
