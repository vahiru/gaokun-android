/*
 * speakerfx.cpp -- libgaokunspeakerfx.so, the speaker processing behind the
 * audio HAL's playback hook (patches/android/hardware-interfaces/0009).
 *
 * WHERE IT RUNS
 * The AIDL audio HAL (StreamPrimary) hands every playback block to
 * playback_hook_process() right before writing it into ALSA, in place.  That
 * is after AudioFlinger has mixed everything, games' FAST tracks included --
 * which no session effect ever sees (AudioFlinger refuses software effects on
 * a FAST session, "non HW effect ... in fast mode") -- so the bass chain and
 * its ceiling cover every sound the speakers play.  The HAL finds this
 * library through ro.vendor.audio.primary.playback_hook (device.mk).
 *
 * THE CONTRACT (C linkage, all on the stream's worker thread)
 *   void *playback_hook_open(int card, int device, int sampleRate,
 *                            int channelCount, int pcmFormat);
 *       Called when the stream opens its ALSA device.  nullptr means "not
 *       mine": the HAL then never calls process() for that stream.  This
 *       library takes the built-in speaker PCM only (card 0, device 1, see
 *       bin/audio-route.sh), 48 kHz stereo, S16_LE or S32_LE (tinyalsa's
 *       enum pcm_format: 0 and 1).
 *   void playback_hook_process(void *handle, void *buffer, size_t frames);
 *       In place.  No allocation, no lock, bounded time (one mixer read
 *       every 100 ms).
 *   void playback_hook_close(void *handle);
 *       Called on standby / shutdown / device change.
 * Open and close also move the mixer (MIXER BOOST below).
 *
 * WHEN IT PROCESSES
 * Only while the speaker enhancement is on (vendor.gaokun3.histen.on = 1,
 * init's mirror of the Parts switch) and the processing mode next to it is
 * not Histen (vendor.gaokun3.speakerfx.engine, init's mirror of
 * persist.sys.gaokun3.speakerfx.engine: unset or "bass" -- the default --
 * means this chain).  The session effect applies the same rule
 * (gaokun_effect.cpp), so nothing is processed twice.  Otherwise the buffer
 * is left alone -- except for the mixer boost below.  The switch, the mode
 * and the boost are re-read every 100 ms; the effect re-reads the mode every
 * 16 buffers, so changing it mid-stream can process a stream twice (or not
 * at all) for well under a second, with this chain's limiter still holding
 * the ceiling.
 *
 * MIXER BOOST
 * The chain's -1 dBFS ceiling is digital full scale, but with patches
 * 0113-0115 bin/audio-route.sh sets the WSA digital volume to 78 (-6 dB), so
 * the chain would stop 6 dB short of what the amplifiers can do.  This library
 * therefore raises the mixer itself: when a speaker stream opens while the
 * chain is wanted, it sets both "WSA_RXn Digital Volume" controls to 84
 * (unity gain), and when the last such stream closes it puts back the base
 * audio-route.sh published in vendor.gaokun3.audioroute.spkdig.  Every block
 * is compensated by what the mixer actually stands above that base (read
 * back with the switch, every 100 ms): the chain takes it off its makeup, a
 * passthrough block (switched off or to Histen mid-stream) is attenuated by
 * it.  So no sound is louder than at the base, and
 *   - the mixer moves only at open and close, with nothing playing; a switch
 *     mid-stream changes the processing, the mixer follows at the next
 *     playback;
 *   - a stream opened with the chain not wanted leaves the mixer at the base
 *     and passes through bit-exact;
 *   - if this library is not loaded, nothing raises the mixer.
 * Without the published base (audio-route.sh has not run yet) the mixer is
 * left alone and nothing is compensated.  The HAL opens the same control
 * device for its own mixer (alsa/Mixer.cpp).
 *
 * KNOBS (re-read about once a second; vendor_gaokun3_prop, root to set)
 *   persist.vendor.gaokun3.speakerfx.bass.lowboost  dB, 0..24        (14)
 *   persist.vendor.gaokun3.speakerfx.bass.lowhp     Hz, 0 or 20..120 (50)
 *   persist.vendor.gaokun3.speakerfx.bass.lowsub    dB, -99..12      (10; <= -60 removes it)
 *   persist.vendor.gaokun3.speakerfx.bass.trim      dB, -12..0       (-3)
 *   persist.vendor.gaokun3.speakerfx.bass.harm      dB, -99..0       (-9; <= -60 off)
 *   persist.vendor.gaokun3.speakerfx.bass.lc        dB/dB, 0..1      (0.15)
 *   persist.vendor.gaokun3.speakerfx.bass.lcmax     dB, 0..18        (12)
 *   persist.vendor.gaokun3.speakerfx.bass.mbc       0 / 1            (1)
 *   persist.vendor.gaokun3.speakerfx.bass.xtc       0..0.9           (0.55; 0 off)
 *   persist.vendor.gaokun3.speakerfx.bass.xtcdist   cm, 15..200      (30)
 *   persist.vendor.gaokun3.speakerfx.bass.xtclo     Hz, 100..2000    (250)
 *   persist.vendor.gaokun3.speakerfx.bass.makeup    dB, -20..12      (4, before the boost)
 *   vendor.gaokun3.speakerfx.ref                    1 = A/B reference (not persistent)
 *
 * Built standalone with GAOKUN_SPKFX_MASTER_ONLY (the KernelSU test module
 * for 1.0.0-rc.2, whose effect does not know the processing mode) the mode
 * is not checked.
 *
 * Log tag gaokun_spkfx: one line per open / state change / knob change, and
 * while processing one line every ~2 s (levels, loudness compensation, gain
 * reduction, boost, cost).
 */
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <sound/asound.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#include "bass_chain.h"
#include "echo_reference.h"

namespace {

constexpr char kTag[] = "gaokun_spkfx";

constexpr int kSpeakerCard = 0;
constexpr int kSpeakerDevice = 1;
constexpr int kFormatS16 = 0;   /* tinyalsa PCM_FORMAT_S16_LE */
constexpr int kFormatS32 = 1;   /* tinyalsa PCM_FORMAT_S32_LE */

constexpr unsigned kGateFrames = 4800;       /* switch, mode and boost every 100 ms */
constexpr unsigned kRefreshFrames = 48000;   /* all knobs about once a second */
constexpr unsigned kStatFrames = 96000;      /* a log line every ~2 s */

constexpr char kMasterProp[] = "vendor.gaokun3.histen.on";
constexpr char kEngineProp[] = "vendor.gaokun3.speakerfx.engine";
constexpr char kBaseProp[] = "vendor.gaokun3.audioroute.spkdig";
constexpr char kRefProp[] = "vendor.gaokun3.speakerfx.ref";
#define BASS_PROP(name) "persist.vendor.gaokun3.speakerfx.bass." name

constexpr char kCtlDevice[] = "/dev/snd/controlC0";   /* kSpeakerCard */
constexpr const char *kDigCtls[] = {"WSA_RX0 Digital Volume", "WSA_RX1 Digital Volume"};
constexpr int kUnityDig = 84;   /* 0 dB at 1 dB a step, and the kernel's cap (audio-route.sh) */

/* Open speaker streams that raised the mixer; the last one out puts it back. */
std::mutex gDigLock;
int gDigRaised = 0;

void logi(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, kTag, fmt, ap);
    va_end(ap);
}

bool propIs(const char *name, const char *value) {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, buf) <= 0) return false;
    return std::strcmp(buf, value) == 0;
}

/* Unset, unparsable or out of [lo, hi] gives `fallback`: a stray value must
 * never reach the DSP. */
double prop(const char *name, double lo, double hi, double fallback) {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, buf) <= 0) return fallback;
    char *end = nullptr;
    const double v = std::strtod(buf, &end);
    if (end == buf || !(v == v) || v < lo || v > hi) return fallback;
    return v;
}

/* One WSA digital volume control, through the ALSA control device by name --
 * what tinymix does, minus the enumeration: with numid 0 the kernel looks the
 * control up by interface and name.  Both controls hold one integer. */
bool digCtl(int fd, unsigned req, const char *name, long *value) {
    snd_ctl_elem_value v;
    std::memset(&v, 0, sizeof(v));
    v.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    std::strncpy(reinterpret_cast<char *>(v.id.name), name, sizeof(v.id.name) - 1);
    if (req == SNDRV_CTL_IOCTL_ELEM_WRITE) v.value.integer.value[0] = *value;
    if (ioctl(fd, req, &v) < 0) return false;
    if (req == SNDRV_CTL_IOCTL_ELEM_READ) *value = v.value.integer.value[0];
    return true;
}

/* The higher of the two (compensating by it keeps both channels at or under
 * the base), or -1 if either cannot be read. */
int readDig(int fd) {
    long hi = -1;
    for (const char *name : kDigCtls) {
        long v = 0;
        if (!digCtl(fd, SNDRV_CTL_IOCTL_ELEM_READ, name, &v)) return -1;
        if (v > hi) hi = v;
    }
    return static_cast<int>(hi);
}

bool writeDig(int fd, int value) {
    bool ok = true;
    for (const char *name : kDigCtls) {
        long v = value;
        ok = digCtl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, name, &v) && ok;
    }
    return ok;
}

bool wanted() {
#ifdef GAOKUN_SPKFX_MASTER_ONLY
    (void)kEngineProp;
    return propIs(kMasterProp, "1");
#else
    return propIs(kMasterProp, "1") && !propIs(kEngineProp, "histen");
#endif
}

/* Knob values; `boostDb` is taken off the makeup. */
gaokun::BassParams readParams(double boostDb) {
    gaokun::BassParams p;
    p.reference = propIs(kRefProp, "1");
    p.lowBoostDb = prop(BASS_PROP("lowboost"), 0.0, 24.0, p.lowBoostDb);
    p.lowHpHz = prop(BASS_PROP("lowhp"), 0.0, 120.0, p.lowHpHz);
    if (p.lowHpHz > 0.0 && p.lowHpHz < 20.0) p.lowHpHz = 20.0;
    p.lowSubDb = prop(BASS_PROP("lowsub"), -99.0, 12.0, p.lowSubDb);
    p.baseTrimDb = prop(BASS_PROP("trim"), -12.0, 0.0, p.baseTrimDb);
    p.vbGainDb = prop(BASS_PROP("harm"), -99.0, 0.0, p.vbGainDb);
    p.lcSlope = prop(BASS_PROP("lc"), 0.0, 1.0, p.lcSlope);
    p.lcMaxDb = prop(BASS_PROP("lcmax"), 0.0, 18.0, p.lcMaxDb);
    p.mbcOn = !propIs(BASS_PROP("mbc"), "0");
    p.xtcK = prop(BASS_PROP("xtc"), 0.0, 0.9, p.xtcK);
    p.xtcDistCm = prop(BASS_PROP("xtcdist"), 15.0, 200.0, p.xtcDistCm);
    p.xtcLoHz = prop(BASS_PROP("xtclo"), 100.0, 2000.0, p.xtcLoHz);
    p.makeupDb = prop(BASS_PROP("makeup"), -20.0, 12.0, p.makeupDb) - boostDb;
    return p;
}

/* The fields readParams() sets -- the only ones that can change. */
bool sameKnobs(const gaokun::BassParams &a, const gaokun::BassParams &b) {
    return a.reference == b.reference && a.lowBoostDb == b.lowBoostDb &&
           a.lowHpHz == b.lowHpHz && a.lowSubDb == b.lowSubDb && a.baseTrimDb == b.baseTrimDb &&
           a.vbGainDb == b.vbGainDb && a.lcSlope == b.lcSlope && a.lcMaxDb == b.lcMaxDb &&
           a.mbcOn == b.mbcOn && a.xtcK == b.xtcK && a.xtcDistCm == b.xtcDistCm &&
           a.xtcLoHz == b.xtcLoHz && a.makeupDb == b.makeupDb;
}

void logParams(const char *why, const gaokun::BassParams &p, double boostDb) {
    logi("%s: ref=%d lowboost=%.1f lowhp=%.0f lowsub=%.1f trim=%.1f harm=%.1f lc=%.2f/max%.0f "
         "mbc=%d xtc=%.2f@%.0fcm/%.0fHz makeup=%+.1fdB (boost %.0fdB taken off)",
         why, p.reference ? 1 : 0, p.lowBoostDb, p.lowHpHz, p.lowSubDb, p.baseTrimDb, p.vbGainDb,
         p.lcSlope, p.lcMaxDb, p.mbcOn ? 1 : 0, p.xtcK, p.xtcDistCm, p.xtcLoHz, p.makeupDb, boostDb);
}

double nowUs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec * 1e-3;
}

struct Stream {
    int format = kFormatS16;
    double scale = 32768.0;          /* full scale of the sample format */
    bool active = false;             /* the chain runs */
    double boostDb = 0.0;            /* mixer above the base */
    double boostGain = 1.0;          /* attenuation while inactive and boosted */
    int ctlFd = -1;                  /* control device; -1 = the mixer is not ours */
    int digBase = -1;
    bool digRaised = false;          /* counted in gDigRaised */
    unsigned sinceRefresh = 0;
    unsigned sinceGate = 0;
    gaokun::BassChain chain;
    gaokun::BassParams params;

    /* stats for the log line */
    unsigned statFrames = 0, statCalls = 0;
    double statPeakIn = 0.0, statPeakOut = 0.0, statUs = 0.0, statMaxUs = 0.0;

    /* At open nothing is playing yet, so the mixer can move without a jump. */
    void attachMixer() {
        digBase = static_cast<int>(prop(kBaseProp, 0.0, kUnityDig, -1.0));
        if (digBase < 0 || digBase == kUnityDig) return;   /* unknown, or nothing to borrow */
        ctlFd = open(kCtlDevice, O_RDWR | O_CLOEXEC);
        if (ctlFd < 0) {
            logi("mixer: %s: %s -- not borrowing", kCtlDevice, std::strerror(errno));
            return;
        }
        std::lock_guard<std::mutex> lock(gDigLock);
        if (wanted()) {
            digRaised = true;
            gDigRaised++;
        }
        const int want = gDigRaised > 0 ? kUnityDig : digBase;
        const int now = readDig(ctlFd);
        if (now != want) {
            if (writeDig(ctlFd, want)) {
                logi("mixer: WSA digital %d -> %d (base %d)", now, want, digBase);
            } else {
                logi("mixer: setting WSA digital %d failed: %s", want, std::strerror(errno));
            }
        }
    }

    void detachMixer() {
        if (ctlFd < 0) return;
        {
            std::lock_guard<std::mutex> lock(gDigLock);
            if (digRaised && --gDigRaised == 0) {
                if (writeDig(ctlFd, digBase)) {
                    logi("mixer: WSA digital back to %d", digBase);
                } else {
                    logi("mixer: restoring WSA digital %d failed: %s", digBase,
                         std::strerror(errno));
                }
            }
        }
        close(ctlFd);
        ctlFd = -1;
    }

    /* What the mixer stands above the base, read back.  A failed read keeps
     * the last value: guessing 0 could make the speakers louder. */
    double mixerBoostDb() const {
        if (ctlFd < 0) return 0.0;
        const int dig = readDig(ctlFd);
        if (dig < 0) return boostDb;
        return dig > digBase ? dig - digBase : 0.0;
    }

    void refresh(bool log) {
        boostDb = mixerBoostDb();
        boostGain = std::pow(10.0, -boostDb / 20.0);
        const bool want = wanted();
        const gaokun::BassParams p = readParams(boostDb);
        if (want != active) {
            active = want;
            params = p;
            chain.configure(p);
            chain.reset();
            logi("%s (boost %.0f dB)", active ? "processing" : "passthrough", boostDb);
            if (active) logParams("knobs", p, boostDb);
        } else if (active && !sameKnobs(p, params)) {
            params = p;
            chain.configure(p);
            logParams("knobs changed", p, boostDb);
        } else if (log && active) {
            logParams("knobs", p, boostDb);
        }
    }

    double read(const void *buf, size_t i) const {
        return format == kFormatS16 ? static_cast<const int16_t *>(buf)[i] / scale
                                    : static_cast<const int32_t *>(buf)[i] / scale;
    }

    void write(void *buf, size_t i, double v) const {
        const double s = std::nearbyint(v * scale);
        if (format == kFormatS16) {
            static_cast<int16_t *>(buf)[i] =
                    static_cast<int16_t>(s > 32767.0 ? 32767.0 : (s < -32768.0 ? -32768.0 : s));
        } else {
            static_cast<int32_t *>(buf)[i] = static_cast<int32_t>(
                    s > 2147483647.0 ? 2147483647.0 : (s < -2147483648.0 ? -2147483648.0 : s));
        }
    }

    void process(void *buf, size_t frames) {
        if (sinceRefresh == 0) {
            refresh(false);
        } else if (sinceGate == 0 && (wanted() != active ||
                                      mixerBoostDb() != boostDb)) {
            refresh(false);
        }
        sinceRefresh += static_cast<unsigned>(frames);
        sinceGate += static_cast<unsigned>(frames);
        if (sinceRefresh >= kRefreshFrames) sinceRefresh = 0;
        if (sinceGate >= kGateFrames) sinceGate = 0;

        if (!active) {
            if (boostDb > 0.0) {
                for (size_t i = 0; i < frames * 2; i++) write(buf, i, read(buf, i) * boostGain);
            }
            return;
        }

        const double t0 = nowUs();
        double pin = statPeakIn, pout = statPeakOut;
        for (size_t i = 0; i < frames; i++) {
            const double l = read(buf, 2 * i), r = read(buf, 2 * i + 1);
            double ol, orr;
            chain.step(l, r, ol, orr);
            write(buf, 2 * i, ol);
            write(buf, 2 * i + 1, orr);
            const double a = std::fmax(std::fabs(l), std::fabs(r));
            const double b = std::fmax(std::fabs(ol), std::fabs(orr));
            if (a > pin) pin = a;
            if (b > pout) pout = b;
        }
        const double us = nowUs() - t0;
        statPeakIn = pin;
        statPeakOut = pout;
        statUs += us;
        if (us > statMaxUs) statMaxUs = us;
        statCalls++;
        statFrames += static_cast<unsigned>(frames);
        if (statFrames >= kStatFrames) {
            const auto grDb = [](double g) { return -20.0 * std::log10(g > 1e-3 ? g : 1e-3); };
            logi("meter: peak in %.3f out %.3f, lc %+.1f dB, bass-gr %.1f dB, gr %.1f dB, "
                 "boost %.0f dB, ref %d, %.0f us/block avg %.0f max (%.1f%% of real time)",
                 statPeakIn, statPeakOut, chain.loudnessCompDb(), grDb(chain.minBassGain()),
                 grDb(chain.minFullGain()), boostDb, params.reference ? 1 : 0,
                 statUs / statCalls, statMaxUs, 100.0 * statUs / (statFrames / 48.0 * 1000.0));
            chain.resetStats();
            statFrames = statCalls = 0;
            statPeakIn = statPeakOut = statUs = statMaxUs = 0.0;
        }
    }
};

/*
 * ECHO REFERENCE
 * Every block this library hands back -- whatever it did to it, so exactly what ALSA plays on the
 * speakers -- also goes into one ring as mono float, for the capture hook's echo canceller
 * (echo_reference.h, micarray/README.md).  A copy and an average per frame; no lock, no wait.
 */
gaokun::echoref::EchoReference &echoReference() {
    /* Constant-initialised and trivially destructible: in .bss, no allocation, no guard. */
    static gaokun::echoref::EchoReference r;
    return r;
}

void tapEchoReference(const Stream &s, const void *buf, size_t frames) {
    gaokun::echoref::EchoReference &r = echoReference();
    uint64_t n = r.written.load(std::memory_order_relaxed);
    for (size_t i = 0; i < frames; i++, n++) {
        r.mono[n % gaokun::echoref::EchoReference::kFrames] =
                static_cast<float>(0.5 * (s.read(buf, 2 * i) + s.read(buf, 2 * i + 1)));
    }
    r.lastWriteNs.store(static_cast<int64_t>(nowUs() * 1000.0), std::memory_order_relaxed);
    r.written.store(n, std::memory_order_release);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void *playback_hook_open(
        int card, int device, int sampleRate, int channelCount, int pcmFormat) {
    if (card != kSpeakerCard || device != kSpeakerDevice) return nullptr;
    if (sampleRate != 48000 || channelCount != 2 ||
        (pcmFormat != kFormatS16 && pcmFormat != kFormatS32)) {
        logi("speaker stream %d Hz, %d ch, format %d: not processed (48 kHz stereo S16/S32 only)",
             sampleRate, channelCount, pcmFormat);
        return nullptr;
    }
    Stream *s = new (std::nothrow) Stream();
    if (s == nullptr) return nullptr;
    s->format = pcmFormat;
    s->scale = pcmFormat == kFormatS16 ? 32768.0 : 2147483648.0;
    logi("speaker stream open: format %d", pcmFormat);
    s->attachMixer();
    s->refresh(true);
    s->sinceRefresh = 1;   /* just refreshed */
    return s;
}

extern "C" __attribute__((visibility("default"))) void playback_hook_process(void *handle,
                                                                             void *buffer,
                                                                             size_t frames) {
    if (handle == nullptr || buffer == nullptr || frames == 0) return;
    Stream *s = static_cast<Stream *>(handle);
    s->process(buffer, frames);
    tapEchoReference(*s, buffer, frames);
}

extern "C" __attribute__((visibility("default"))) gaokun::echoref::EchoReference *
playback_hook_echo_reference(void) {
    return &echoReference();
}

extern "C" __attribute__((visibility("default"))) void playback_hook_close(void *handle) {
    if (handle == nullptr) return;
    logi("speaker stream close");
    Stream *s = static_cast<Stream *>(handle);
    s->detachMixer();
    delete s;
}
