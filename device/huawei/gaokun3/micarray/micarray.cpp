/*
 * Microphone array processing for the Huawei MateBook E Go (gaokun3), loaded by the audio HAL's
 * capture hook (patches/android/hardware-interfaces/0010, ro.vendor.audio.primary.capture_hook in device.mk).  See README.md.
 *
 * The built-in microphone stream is captured as four channels, decimator n = DMICn, instead of
 * two (DMIC0 and DMIC1).  The three microphones along the top edge -- DMIC1, DMIC3, DMIC2 from
 * left to right in landscape -- go through a fixed beamformer (beamformer.h) into the stereo
 * pair the client gets, kLatencyFrames (30 ms) later.
 *
 *   void *capture_hook_open(int card, int device, int sampleRate, int channelCount,
 *                           int pcmFormat, const int *sources, size_t sourceCount,
 *                           int *hardwareChannelCount, int *delayFrames);
 *       Takes the built-in microphone (card 0, device 3, 48 kHz stereo S16) when every record
 *       source wants processed audio, the switch is not off and the weights load; adds
 *       decimators 2 and 3 to the mixer.  Anything else: nullptr, captured as without the hook
 *       (after taking out decimators 2 and 3 if a crashed HAL left them in).
 *   void capture_hook_process(void *handle, const void *in, void *out, size_t frames);
 *       Four channels in, two out.  No allocation, no lock.
 *   void capture_hook_set_sources(void *handle, const int *sources, size_t sourceCount);
 *       A source that wants raw audio joining the stream (UNPROCESSED, ...) switches the output
 *       to DMIC0/DMIC1 -- what two-channel capture gets -- delayed to the same 30 ms so that
 *       the stream's timing does not jump, with a 10 ms cross-fade.  Back when it leaves.
 *       A stream of calls only (VOICE_COMMUNICATION) gets the array's two outputs averaged on
 *       both channels, the mono a call uses anyway; it also fades in and out.
 *   void capture_hook_close(void *handle);
 *       The last stream out puts the mixer back.
 *
 * A stream that opens with a raw source captures without the hook until its next start: the
 * HAL fixes the channel count and the delay at open.
 *
 * Echo cancellation: a stream of calls only (at open) also goes through WebRTC's AEC3
 * (echo_canceller.h), on the mono average of the array's outputs, with what the speakers played as
 * the far end (speakerfx's echo reference, echo_reference_reader.h).  The platform's echo
 * canceller never sees capture on this HAL (README.md), so this is the only one apps get.  The
 * stream then stays mono and echo-cancelled to its end, whoever joins it (but a raw source, which
 * switches it to the raw pair like any stream), and its delay grows by AEC3's 430 frames.  persist.vendor.gaokun3.aec=0 turns it off from the next start.
 *
 * Noise suppression: when every record source of a stream belongs to a scene whose switch is on
 * (vendor.gaokun3.denoise.<scene>, read at open: "call" on unless 0, "record" and "camcorder" off
 * unless 1) and the noise suppression service (denoise_protocol.h) answers at open, the stream
 * goes through DeepFilterNet3 there -- a call stream as one channel after the echo canceller,
 * others as the array's two outputs -- and its delay grows by 30 ms.  Sources leaving the
 * switched-on scenes mid-stream fade to the unfiltered output, delayed to the same time; a service
 * that fails or answers late does the same for the rest of the stream.
 */

#include <android/log.h>
#include <fcntl.h>
#include <poll.h>
#include <sound/asound.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

#include "beamformer.h"
#include "denoise_protocol.h"
#include "echo_canceller.h"
#include "echo_reference_reader.h"

namespace {

using namespace gaokun::micarray;
namespace dn = gaokun::micarray::denoise;

constexpr char kTag[] = "gaokun_micarray";

constexpr int kCard = 0;
constexpr int kDevice = 3;
constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr int kFormatS16 = 0;  // tinyalsa PCM_FORMAT_S16_LE
constexpr size_t kHardwareChannels = 4;
// Channels of the four-channel capture: the top edge from left to right, and the pair
// two-channel capture gets.
constexpr std::array<size_t, kInputs> kArrayChannels = {1, 3, 2};
constexpr size_t kRawChannels[kOutputs] = {0, 1};
constexpr size_t kFadeFrames = kHop;
static_assert(dn::kRate == kRate && dn::kHop == kHop && dn::kMaxChannels == kOutputs);
constexpr size_t kDenoiseLatencyFrames = dn::kLatencyHops * kHop;
constexpr size_t kMaxLatencyFrames =
        kLatencyFrames + EchoCanceller::kLatencyFrames + kDenoiseLatencyFrames;
static_assert(EchoCanceller::kFrame == kHop);
// Speaker samples taken from the echo reference at a time: a capture block of 4096 frames
// normally finds about as many.
constexpr size_t kRenderPull = 8192;
// process() works through its input in chunks of at most this many hops of frames; a chunk
// completes at most kChunkHops + 1 hops, all sent to the noise suppression service in one message.
constexpr size_t kChunkHops = 12;
static_assert(kChunkHops + 1 <= dn::kMaxHops);
// Output hops kept by hop number: a chunk's new ones, the one handed out when it started, and
// the noise suppression delay before that.
constexpr size_t kRing = 32;
static_assert(kRing >= kChunkHops + 2 + dn::kLatencyHops);
// How long open waits for the service's answer; it answers at once when it is there.
constexpr int kDenoiseHelloMs = 200;

constexpr char kSwitchProp[] = "persist.vendor.gaokun3.micarray";
#ifndef GAOKUN_MICARRAY_WEIGHTS  // tests load the weights from elsewhere
#define GAOKUN_MICARRAY_WEIGHTS "/vendor/etc/micarray/hdrec_bf_data.bin"
#endif
constexpr char kWeightsPath[] = GAOKUN_MICARRAY_WEIGHTS;
constexpr char kCtlDevice[] = "/dev/snd/controlC0";  // kCard
#ifdef GAOKUN_DENOISE_SOCKET  // tests run their own service
constexpr const char* kDenoiseSocket = GAOKUN_DENOISE_SOCKET;
#else
constexpr const char* kDenoiseSocket = dn::kSocketPath;
#endif

void logi(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, kTag, fmt, ap);
    va_end(ap);
}

bool propIs(const char* name, const char* value) {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, buf) <= 0) return false;
    return std::strcmp(buf, value) == 0;
}

double nowUs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec * 1e-3;
}

// AudioSource values that get the array: everything recorded for people to listen to or for
// speech recognition.  UNPROCESSED, VOICE_PERFORMANCE and the rest get the raw pair.
bool wantsArray(int source) {
    switch (source) {
        case 0:  // DEFAULT
        case 1:  // MIC
        case 5:  // CAMCORDER
        case 6:  // VOICE_RECOGNITION
        case 7:  // VOICE_COMMUNICATION
            return true;
        default:
            return false;
    }
}

// Calls are mono; the two outputs are in phase for a talker in front (within 15 degrees below
// 16 kHz), so their average loses nothing and halves what is uncorrelated between them.
bool allCalls(const int* sources, size_t count) {
    if (count == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        if (sources[i] != 7) return false;  // VOICE_COMMUNICATION
    }
    return true;
}

const char* modeName(bool raw, bool mono) {
    return raw ? "raw pair, delayed" : mono ? "array, mono" : "array";
}

bool allWantArray(const int* sources, size_t count) {
    if (count == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!wantsArray(sources[i])) return false;
    }
    return true;
}

// Noise suppression scenes by AudioSource; each has a switch vendor.gaokun3.denoise.<name>.
// VOICE_RECOGNITION has none: the CDD (5.4.2 C-1-2) wants noise reduction off for it.
enum Scene : unsigned { kSceneCall = 1, kSceneRecord = 2, kSceneCamcorder = 4 };

unsigned sceneOf(int source) {
    switch (source) {
        case 7:  // VOICE_COMMUNICATION
            return kSceneCall;
        case 0:  // DEFAULT
        case 1:  // MIC
            return kSceneRecord;
        case 5:  // CAMCORDER
            return kSceneCamcorder;
        default:
            return 0;
    }
}

// The switches: calls on unless set to 0, the others off unless set to 1.
unsigned denoiseScenes() {
    unsigned scenes = 0;
    if (!propIs("vendor.gaokun3.denoise.call", "0")) scenes |= kSceneCall;
    if (propIs("vendor.gaokun3.denoise.record", "1")) scenes |= kSceneRecord;
    if (propIs("vendor.gaokun3.denoise.camcorder", "1")) scenes |= kSceneCamcorder;
    return scenes;
}

bool allDenoised(const int* sources, size_t count, unsigned scenes) {
    if (count == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        if ((sceneOf(sources[i]) & scenes) == 0) return false;
    }
    return true;
}

std::string sourceList(const int* sources, size_t count) {
    std::string s;
    for (size_t i = 0; i < count; ++i) s += (i ? "," : "") + std::to_string(sources[i]);
    return s.empty() ? "none" : s;
}

const Weights* weights() {
    static const Weights* const w = []() -> const Weights* {
        auto* loaded = new (std::nothrow) Weights;
        std::string error;
        if (loaded == nullptr || !loadWeights(kWeightsPath, loaded, &error)) {
            logi("weights: %s: %s -- array off", kWeightsPath, loaded ? error.c_str() : "no memory");
            delete loaded;
            return nullptr;
        }
        logi("weights: %s loaded", kWeightsPath);
        return loaded;
    }();
    return w;
}

/*
 * MIXER
 *
 * Decimator n takes DMICn into the capture DAI.  audio-route.sh routes 0 and 1 at boot for
 * two-channel capture.  2 and 3 are added at the first array open, with the gain of decimator 1
 * (the left top microphone), and their paths into the DAI are taken out at the last close, so
 * that every other capture runs as it does without this library.  DMIC3 gets its clock only
 * together with DMIC2.
 *
 * The paths are taken out by writing 0, not by putting back a saved state: the mixer outlives
 * the HAL process, and after a crash the saved state would be a stale "in".  For the same reason
 * every open of the built-in microphone with no array stream open first takes out paths left in.
 * The routing and gain of decimators 2 and 3 do nothing while their paths are out; they are put
 * back as found.  Controls are addressed by name through the control device, like tinymix does.
 */

bool ctlInfo(int fd, const char* name, snd_ctl_elem_info* info) {
    std::memset(info, 0, sizeof(*info));
    info->id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    std::strncpy(reinterpret_cast<char*>(info->id.name), name, sizeof(info->id.name) - 1);
    return ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, info) == 0;
}

// The first value of an integer, boolean or enumerated control (the item index for an enum).
bool ctlGet(int fd, const char* name, long* value) {
    snd_ctl_elem_info info;
    if (!ctlInfo(fd, name, &info)) return false;
    snd_ctl_elem_value v;
    std::memset(&v, 0, sizeof(v));
    v.id = info.id;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0) return false;
    *value = info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED ? v.value.enumerated.item[0]
                                                         : v.value.integer.value[0];
    return true;
}

bool ctlSet(int fd, const char* name, long value) {
    snd_ctl_elem_info info;
    if (!ctlInfo(fd, name, &info)) return false;
    snd_ctl_elem_value v;
    std::memset(&v, 0, sizeof(v));
    v.id = info.id;
    if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
        v.value.enumerated.item[0] = static_cast<unsigned int>(value);
    } else {
        v.value.integer.value[0] = value;
    }
    return ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) == 0;
}

// The index of item 'item' of enumerated control 'name', or -1.
long ctlEnumIndex(int fd, const char* name, const char* item) {
    snd_ctl_elem_info info;
    if (!ctlInfo(fd, name, &info) || info.type != SNDRV_CTL_ELEM_TYPE_ENUMERATED) return -1;
    for (unsigned int i = 0; i < info.value.enumerated.items; ++i) {
        info.value.enumerated.item = i;
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) return -1;
        if (std::strcmp(info.value.enumerated.name, item) == 0) return i;
    }
    return -1;
}

struct Setting {
    char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
    long value;  // wanted, then what it was before
};

// Array streams open; the first adds decimators 2 and 3, the last takes them out.
std::mutex gMixerLock;
int gMixerUsers = 0;
// Routing and gain of decimators 2 and 3 before the first array open.
Setting gSaved[6];
size_t gSavedCount = 0;

bool takeOutPaths(int fd) {
    bool ok = true;
    for (unsigned int n = 2; n < kHardwareChannels; ++n) {
        char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
        std::snprintf(name, sizeof(name), "VA_AIF1_CAP Mixer DEC%u", n);
        ok = ctlSet(fd, name, 0) && ok;
    }
    return ok;
}

void restoreSaved(int fd) {
    for (size_t i = gSavedCount; i-- > 0;) ctlSet(fd, gSaved[i].name, gSaved[i].value);
    gSavedCount = 0;
}

// Whether decimator n takes DMICn into the DAI.
bool routed(int fd, unsigned int n) {
    char mux[SNDRV_CTL_ELEM_ID_NAME_MAXLEN], dmic[SNDRV_CTL_ELEM_ID_NAME_MAXLEN],
            path[SNDRV_CTL_ELEM_ID_NAME_MAXLEN], item[8];
    std::snprintf(mux, sizeof(mux), "VA DEC%u MUX", n);
    std::snprintf(dmic, sizeof(dmic), "VA DMIC MUX%u", n);
    std::snprintf(path, sizeof(path), "VA_AIF1_CAP Mixer DEC%u", n);
    std::snprintf(item, sizeof(item), "DMIC%u", n);
    long v = 0;
    return ctlGet(fd, mux, &v) && v == ctlEnumIndex(fd, mux, "VA_DMIC") && ctlGet(fd, dmic, &v) &&
           v == ctlEnumIndex(fd, dmic, item) && ctlGet(fd, path, &v) && v == 1;
}

void takeOutStalePaths() {
    std::lock_guard<std::mutex> lock(gMixerLock);
    if (gMixerUsers > 0) return;
    const int fd = open(kCtlDevice, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    long dec2 = 0, dec3 = 0;
    ctlGet(fd, "VA_AIF1_CAP Mixer DEC2", &dec2);
    ctlGet(fd, "VA_AIF1_CAP Mixer DEC3", &dec3);
    if (dec2 != 0 || dec3 != 0) {
        logi("mixer: decimators 2 and 3 were left in (HAL restarted?) -- %s",
             takeOutPaths(fd) ? "taken out" : "taking them out failed");
    }
    close(fd);
}

bool attachMixer() {
    std::lock_guard<std::mutex> lock(gMixerLock);
    if (gMixerUsers > 0) {
        ++gMixerUsers;
        return true;
    }
    const int fd = open(kCtlDevice, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        logi("mixer: %s: %s", kCtlDevice, std::strerror(errno));
        return false;
    }
    // The raw pair and the array's left microphone come from audio-route.sh's routing.
    if (!routed(fd, 0) || !routed(fd, 1)) {
        logi("mixer: decimators 0 and 1 are not routed from DMIC0 and DMIC1 -- not using the array");
        close(fd);
        return false;
    }
    Setting wanted[6];
    size_t count = 0;
    long gain = 0;
    bool ok = ctlGet(fd, "VA_DEC1 Volume", &gain);
    for (unsigned int n = 2; n < kHardwareChannels && ok; ++n) {
        char item[8];
        std::snprintf(item, sizeof(item), "DMIC%u", n);
        Setting& mux = wanted[count++];
        std::snprintf(mux.name, sizeof(mux.name), "VA DEC%u MUX", n);
        mux.value = ctlEnumIndex(fd, mux.name, "VA_DMIC");
        Setting& dmic = wanted[count++];
        std::snprintf(dmic.name, sizeof(dmic.name), "VA DMIC MUX%u", n);
        dmic.value = ctlEnumIndex(fd, dmic.name, item);
        Setting& volume = wanted[count++];
        std::snprintf(volume.name, sizeof(volume.name), "VA_DEC%u Volume", n);
        volume.value = gain;
        ok = mux.value >= 0 && dmic.value >= 0;
    }
    // Routing first, then the paths into the DAI.
    gSavedCount = 0;
    for (size_t i = 0; i < count && ok; ++i) {
        gSaved[i] = wanted[i];
        ok = ctlGet(fd, wanted[i].name, &gSaved[i].value);
        if (ok) {
            ++gSavedCount;
            ok = ctlSet(fd, wanted[i].name, wanted[i].value);
        }
        if (!ok) logi("mixer: %s: %s", wanted[i].name, std::strerror(errno));
    }
    for (unsigned int n = 2; n < kHardwareChannels && ok; ++n) {
        char path[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
        std::snprintf(path, sizeof(path), "VA_AIF1_CAP Mixer DEC%u", n);
        ok = ctlSet(fd, path, 1);
        if (!ok) logi("mixer: %s: %s", path, std::strerror(errno));
    }
    if (ok) {
        logi("mixer: decimators 2 and 3 = DMIC2 and DMIC3 at gain %ld, added", gain);
        gMixerUsers = 1;
    } else {
        takeOutPaths(fd);
        restoreSaved(fd);
    }
    close(fd);
    return ok;
}

void detachMixer() {
    std::lock_guard<std::mutex> lock(gMixerLock);
    if (--gMixerUsers > 0) return;
    const int fd = open(kCtlDevice, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        logi("mixer: %s: %s -- decimators 2 and 3 left in until the next open", kCtlDevice,
             std::strerror(errno));
        return;
    }
    const bool ok = takeOutPaths(fd);
    restoreSaved(fd);
    logi("mixer: decimators 2 and 3 %s", ok ? "taken out" : "NOT taken out");
    close(fd);
}

// A message to or from the noise suppression service, laid out as on the wire.
struct DenoiseMessage {
    dn::BlockHeader header;
    int16_t pcm[size_t{dn::kMaxHops} * kHop * dn::kMaxChannels];
};
static_assert(offsetof(DenoiseMessage, pcm) == sizeof(dn::BlockHeader));
static_assert(sizeof(DenoiseMessage) == dn::kMaxMessageBytes);

struct Stream {
    explicit Stream(const Weights& w) : beamformer(w) {}

    Beamformer beamformer;
    std::atomic<bool> raw{false};       // set_sources() -> process()
    std::atomic<bool> mono{false};      // set_sources() -> process()
    std::atomic<bool> denoised{false};  // set_sources() -> process(): the sources want the filter
    // Shares follow 'raw', 'mono' and 'denoised' over kFadeFrames.  rawShare: 0 array, 1 raw
    // pair; monoShare: 0 the two outputs, 1 their average on both channels; denoiseShare: 0 the
    // array's output, 1 filtered.
    float rawShare = 0;
    float monoShare = 0;
    float denoiseShare = 0;

    // Framing: the input hop being collected, and the array's outputs by hop number (the last
    // kRing hops), handed out one hop later as Beamformer::process does.
    HopIn in{};
    size_t fill = 0;
    int64_t hops = 0;     // input hops completed
    uint64_t frames = 0;  // frames processed
    std::array<HopOut, kRing> out{};

    // A stream that opened with calls only: the average of the array's outputs by hop number,
    // echo-cancelled when 'aec' runs -- what the noise suppression service gets, and with the
    // echo canceller the stream's output.
    bool calls = false;
    std::array<std::array<int16_t, kHop>, kRing> callOut{};
    std::unique_ptr<EchoCanceller> aec;
    EchoReferenceReader echoRef;
    std::array<float, kRenderPull> pulled{};
    std::array<float, kHop> render{};  // the speaker frame being collected
    size_t renderFill = 0;

    // Noise suppression: the connection (-1: none, or dropped after a failure), the scenes
    // switched on at open, and the filtered hops by hop number, valid below 'filteredEnd'.
    // A stream opened with the filter keeps its delay: without it the array's output is handed
    // out kLatencyHops later instead.
    int denoiseFd = -1;
    bool withDenoise = false;
    uint16_t denoiseChannels = 0;  // 1 for a call stream, 2 otherwise
    unsigned scenes = 0;
    std::array<HopOut, kRing> filtered{};
    int64_t filteredEnd = 0;
    uint32_t seq = 0;
    DenoiseMessage message{};
    uint64_t messages = 0;
    double roundTotalUs = 0;
    double roundMaxUs = 0;

    // The raw pair over the stream's delay, oldest at 'delayPos'.
    size_t latency = kLatencyFrames;
    std::array<int16_t, kMaxLatencyFrames * kOutputs> delay{};
    size_t delayPos = 0;

    uint64_t blocks = 0;
    double totalUs = 0;
    double maxUs = 0;
};

// One frame of a 0..1 cross-fade towards 'on'.
float towards(float share, bool on) {
    return on ? std::min(1.0f, share + 1.0f / kFadeFrames)
              : std::max(0.0f, share - 1.0f / kFadeFrames);
}

// Waits until 'fd' is readable, at most 'timeoutMs'.
bool waitReadable(int fd, int timeoutMs) {
    const double deadline = nowUs() + timeoutMs * 1000.0;
    for (;;) {
        pollfd p = {fd, POLLIN, 0};
        const int left = static_cast<int>((deadline - nowUs() + 999) / 1000);
        if (left <= 0) return false;
        const int r = poll(&p, 1, left);
        if (r > 0) return (p.revents & POLLIN) != 0;
        if (r == 0 || errno != EINTR) return false;
    }
}

// Opens a connection to the noise suppression service for 'channels' channels, or returns -1
// and why.
int connectDenoise(uint16_t channels, std::string* why) {
    // Non-blocking: a service that stopped accepting would otherwise hold connect() (and the
    // HAL worker) once its listen backlog is full; the answer is waited for with poll().
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        *why = std::string("socket: ") + std::strerror(errno);
        return -1;
    }
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kDenoiseSocket, sizeof(addr.sun_path) - 1);
    const dn::Hello hello = {dn::kMagic, dn::kVersion, channels, dn::kRate, dn::kHop};
    dn::HelloReply reply = {};
    if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        *why = std::string(kDenoiseSocket) + ": " + std::strerror(errno);
    } else if (send(fd, &hello, sizeof(hello), MSG_NOSIGNAL) != sizeof(hello)) {
        *why = std::string("send: ") + std::strerror(errno);
    } else if (!waitReadable(fd, kDenoiseHelloMs)) {
        *why = "no answer";
    } else if (recv(fd, &reply, sizeof(reply), 0) != sizeof(reply) || reply.magic != dn::kMagic ||
               reply.version != dn::kVersion) {
        *why = "not the expected service";
    } else if (reply.status != dn::kOk) {
        *why = "service status " + std::to_string(reply.status);
    } else if (reply.latencyHops != dn::kLatencyHops || reply.maxHops < kChunkHops + 1) {
        *why = "service latency " + std::to_string(reply.latencyHops) + " hops, at most " +
               std::to_string(reply.maxHops) + " hops a message";
    } else {
        return fd;
    }
    close(fd);
    return -1;
}

// Sends hops first .. s->hops - 1 of the array's output to the service and keeps what comes
// back.  It has 60% of the chunk's duration; a failure drops the connection for good.
void denoise(Stream* s, int64_t first, size_t frames) {
    const size_t hops = static_cast<size_t>(s->hops - first);
    const size_t channels = s->denoiseChannels;
    s->message.header = {s->seq, static_cast<uint16_t>(hops), dn::kOk};
    int16_t* p = s->message.pcm;
    for (size_t j = 0; j < hops; ++j) {
        const size_t r = (first + j) % kRing;
        for (size_t n = 0; n < kHop; ++n) {
            if (channels == 1) {
                *p++ = s->callOut[r][n];
            } else {
                *p++ = s->out[r][0][n];
                *p++ = s->out[r][1][n];
            }
        }
    }
    const auto bytes = static_cast<ssize_t>(sizeof(dn::BlockHeader) + hops * kHop * channels * 2);
    const int timeoutMs = std::max(10, static_cast<int>(frames * 600 / kRate));
    const double startUs = nowUs();
    std::string error;
    ssize_t got = 0;
    if (send(s->denoiseFd, &s->message, bytes, MSG_NOSIGNAL | MSG_DONTWAIT) != bytes) {
        error = std::string("send: ") + std::strerror(errno);
    } else if (!waitReadable(s->denoiseFd, timeoutMs)) {
        error = "no answer within " + std::to_string(timeoutMs) + " ms";
    } else if ((got = recv(s->denoiseFd, &s->message, sizeof(s->message), 0)) != bytes ||
               s->message.header.seq != s->seq || s->message.header.hops != hops) {
        error = "unexpected answer (" + std::to_string(got) + " bytes)";
    } else if (s->message.header.status != dn::kOk) {
        error = "service status " + std::to_string(s->message.header.status);
    }
    const double us = nowUs() - startUs;
    if (!error.empty()) {
        logi("denoise: %s after %.1f ms -- the array's output for the rest of the stream",
             error.c_str(), us / 1000);
        close(s->denoiseFd);
        s->denoiseFd = -1;
        return;
    }
    p = s->message.pcm;
    for (size_t j = 0; j < hops; ++j) {
        HopOut& f = s->filtered[(first + j) % kRing];
        for (size_t n = 0; n < kHop; ++n) {
            f[0][n] = *p++;
            f[1][n] = channels == 1 ? f[0][n] : *p++;
        }
    }
    s->filteredEnd = s->hops;
    ++s->seq;
    ++s->messages;
    s->roundTotalUs += us;
    s->roundMaxUs = std::max(s->roundMaxUs, us);
}

// At most kChunkHops hops of frames: the inputs through the beamformer hop by hop, the new hops
// through the service, then out frame by frame.
// What the speakers played since the last block, into the echo canceller in whole frames:
// before the capture it echoes in.
void feedRender(Stream* s) {
    size_t n;
    while ((n = s->echoRef.pull(s->pulled.data(), s->pulled.size())) > 0) {
        for (size_t i = 0; i < n; ++i) {
            s->render[s->renderFill] = s->pulled[i];
            if (++s->renderFill == kHop) {
                s->aec->render(s->render.data());
                s->renderFill = 0;
            }
        }
    }
}

// A call stream's mono hop h: the average of the array's outputs, echo-cancelled if 'aec' runs.
void callHop(Stream* s, int64_t h) {
    const HopOut& o = s->out[h % kRing];
    std::array<int16_t, kHop>& c = s->callOut[h % kRing];
    if (!s->aec) {
        for (size_t n = 0; n < kHop; ++n) {
            c[n] = static_cast<int16_t>(std::lround((o[0][n] + o[1][n]) * 0.5f));
        }
        return;
    }
    float m[kHop];
    for (size_t n = 0; n < kHop; ++n) m[n] = (o[0][n] + o[1][n]) * (0.5f / 32768.0f);
    s->aec->process(m);
    for (size_t n = 0; n < kHop; ++n) {
        const float v = std::nearbyint(m[n] * 32768.0f);
        c[n] = static_cast<int16_t>(std::min(32767.0f, std::max(-32768.0f, v)));
    }
}

void processChunk(Stream* s, const int16_t* x, int16_t* y, size_t frames) {
    if (s->aec) feedRender(s);
    const int64_t first = s->hops;
    for (size_t f = 0; f < frames; ++f) {
        for (size_t i = 0; i < kInputs; ++i) {
            s->in[i][s->fill] = x[f * kHardwareChannels + kArrayChannels[i]];
        }
        if (++s->fill == kHop) {
            s->beamformer.processHop(s->in, s->out[s->hops % kRing]);
            if (s->calls) callHop(s, s->hops);
            ++s->hops;
            s->fill = 0;
        }
    }
    if (s->denoiseFd >= 0 && s->hops > first) denoise(s, first, frames);

    const bool raw = s->raw.load(std::memory_order_relaxed);
    const bool mono = s->mono.load(std::memory_order_relaxed);
    const bool denoised = s->denoised.load(std::memory_order_relaxed) && s->denoiseFd >= 0;
    const int64_t lag = s->withDenoise ? dn::kLatencyHops : 0;
    for (size_t f = 0; f < frames; ++f) {
        const uint64_t n = s->frames + f;
        // The hop handed out now, its frame, and the array's output that goes with it.
        const int64_t h = static_cast<int64_t>(n / kHop) - 1;
        const size_t k = n % kHop;
        int16_t v[kOutputs] = {0, 0};
        if (h - lag >= 0) {
            for (size_t o = 0; o < kOutputs; ++o) {
                // With the echo canceller a call stream is its output on both channels.
                v[o] = s->aec ? s->callOut[(h - lag) % kRing][k] : s->out[(h - lag) % kRing][o][k];
            }
        }
        int16_t* d = &s->delay[s->delayPos * kOutputs];
        const int16_t delayed[kOutputs] = {d[0], d[1]};
        for (size_t o = 0; o < kOutputs; ++o) d[o] = x[f * kHardwareChannels + kRawChannels[o]];
        if (++s->delayPos == s->latency) s->delayPos = 0;
        s->rawShare = towards(s->rawShare, raw);
        s->monoShare = towards(s->monoShare, mono);
        s->denoiseShare = towards(s->denoiseShare, denoised);
        int16_t* w = &y[f * kOutputs];
        w[0] = v[0];
        w[1] = v[1];
        if (s->rawShare == 0 && s->monoShare == 0 && s->denoiseShare == 0) continue;
        // Filtered hops missing (the service failed): the array's output, cut over at once if the
        // fade has not finished.
        const bool haveFiltered = h >= 0 && h < s->filteredEnd;
        // Filtered first, per channel, then the average: a two-channel filtered stream that turns
        // calls-only still comes out mono.
        float b[kOutputs];
        for (size_t o = 0; o < kOutputs; ++o) {
            b[o] = v[o];
            if (haveFiltered) b[o] += (s->filtered[h % kRing][o][k] - b[o]) * s->denoiseShare;
        }
        const float average = (b[0] + b[1]) * 0.5f;
        for (size_t o = 0; o < kOutputs; ++o) {
            float a = b[o] + (average - b[o]) * s->monoShare;
            a += (delayed[o] - a) * s->rawShare;
            w[o] = static_cast<int16_t>(std::lround(a));
        }
    }
    s->frames += frames;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void* capture_hook_open(
        int card, int device, int sampleRate, int channelCount, int pcmFormat, const int* sources,
        size_t sourceCount, int* hardwareChannelCount, int* delayFrames) {
    if (card != kCard || device != kDevice) return nullptr;
    takeOutStalePaths();
    const std::string list = sourceList(sources, sourceCount);
    if (sampleRate != kRate || channelCount != kChannels || pcmFormat != kFormatS16) {
        logi("open: %d Hz, %d channels, format %d -- not handled", sampleRate, channelCount,
             pcmFormat);
        return nullptr;
    }
    if (propIs(kSwitchProp, "0")) {
        logi("open: sources %s, %s=0 -- raw", list.c_str(), kSwitchProp);
        return nullptr;
    }
    if (!allWantArray(sources, sourceCount)) {
        logi("open: sources %s -- raw", list.c_str());
        return nullptr;
    }
    const Weights* w = weights();
    if (w == nullptr) return nullptr;
    auto* s = new (std::nothrow) Stream(*w);
    if (s == nullptr) return nullptr;
    if (!attachMixer()) {
        delete s;
        return nullptr;
    }
    const bool mono = allCalls(sources, sourceCount);
    s->mono = mono;
    s->monoShare = mono ? 1 : 0;
    s->calls = mono;
    std::string aecNote;
    if (mono) {
        std::string why;
        if (propIs("persist.vendor.gaokun3.aec", "0")) {
            why = "persist.vendor.gaokun3.aec=0";
        } else {
            s->aec = EchoCanceller::create(&why);
            // Here, on the HAL's worker thread: the I/O thread never looks for speakerfx.
            if (s->aec && !s->echoRef.attach(&why)) s->aec.reset();
        }
        aecNote = s->aec ? ", echo cancellation" : ", no echo cancellation: " + why;
    }
    s->scenes = denoiseScenes();
    std::string denoiseNote;
    if (allDenoised(sources, sourceCount, s->scenes)) {
        // A call stream is mono: the service filters the one channel the echo canceller gives.
        s->denoiseChannels = mono ? 1 : kOutputs;
        std::string why;
        s->denoiseFd = connectDenoise(s->denoiseChannels, &why);
        denoiseNote = s->denoiseFd >= 0
                ? ", noise suppression (" + std::to_string(s->denoiseChannels) + " ch)"
                : ", no noise suppression: " + why;
    }
    s->withDenoise = s->denoiseFd >= 0;
    s->denoised = s->withDenoise;
    s->denoiseShare = s->withDenoise ? 1 : 0;
    s->latency = kLatencyFrames + (s->aec ? EchoCanceller::kLatencyFrames : 0) +
                 (s->withDenoise ? kDenoiseLatencyFrames : 0);
    *hardwareChannelCount = kHardwareChannels;
    *delayFrames = static_cast<int>(s->latency);
    logi("open: sources %s -- %s (DMIC%zu,%zu,%zu)%s%s, %zu frames delay", list.c_str(),
         modeName(false, mono), kArrayChannels[0], kArrayChannels[1], kArrayChannels[2],
         aecNote.c_str(), denoiseNote.c_str(), s->latency);
    return s;
}

extern "C" __attribute__((visibility("default"))) void capture_hook_process(void* handle,
                                                                            const void* in,
                                                                            void* out,
                                                                            size_t frames) {
    auto* s = static_cast<Stream*>(handle);
    const double startUs = nowUs();
    const auto* x = static_cast<const int16_t*>(in);
    auto* y = static_cast<int16_t*>(out);
    while (frames > 0) {
        const size_t chunk = std::min(frames, kChunkHops * kHop);
        processChunk(s, x, y, chunk);
        x += chunk * kHardwareChannels;
        y += chunk * kOutputs;
        frames -= chunk;
    }
    const double us = nowUs() - startUs;
    ++s->blocks;
    s->totalUs += us;
    s->maxUs = std::max(s->maxUs, us);
}

extern "C" __attribute__((visibility("default"))) void capture_hook_set_sources(
        void* handle, const int* sources, size_t sourceCount) {
    auto* s = static_cast<Stream*>(handle);
    // No tracks: the stream is about to stop, keep what it has.
    if (sourceCount == 0) return;
    const bool raw = !allWantArray(sources, sourceCount);
    const bool mono = allCalls(sources, sourceCount);
    // A mono filter has nothing to give a stereo stream -- unless the echo canceller keeps the
    // stream mono anyway.
    const bool denoised = s->withDenoise && allDenoised(sources, sourceCount, s->scenes) &&
                          (s->denoiseChannels == kOutputs || mono || s->aec);
    const bool rawChanged = s->raw.exchange(raw) != raw;
    const bool denoisedChanged = s->denoised.exchange(denoised) != denoised;
    if (s->mono.exchange(mono) != mono || rawChanged || denoisedChanged) {
        logi("sources %s -- %s%s", sourceList(sources, sourceCount).c_str(), modeName(raw, mono),
             denoised ? ", noise suppression" : "");
    }
}

extern "C" __attribute__((visibility("default"))) void capture_hook_close(void* handle) {
    auto* s = static_cast<Stream*>(handle);
    logi("close: %llu frames in %llu blocks, %.0f us a block on average, %.0f us at most",
         static_cast<unsigned long long>(s->frames), static_cast<unsigned long long>(s->blocks),
         s->blocks ? s->totalUs / s->blocks : 0.0, s->maxUs);
    if (s->withDenoise) {
        logi("close: noise suppression %s, %llu messages, %.1f ms a round trip on average, "
             "%.1f ms at most",
             s->denoiseFd >= 0 ? "to the end" : "dropped", static_cast<unsigned long long>(s->messages),
             s->messages ? s->roundTotalUs / s->messages / 1000 : 0.0, s->roundMaxUs / 1000);
    }
    if (s->aec) {
        logi("close: echo cancellation, %llu speaker frames (%llu skips), delay estimate %d ms",
             static_cast<unsigned long long>(s->echoRef.frames()),
             static_cast<unsigned long long>(s->echoRef.skips()), s->aec->delayMs());
    }
    if (s->denoiseFd >= 0) close(s->denoiseFd);
    detachMixer();
    delete s;
}
