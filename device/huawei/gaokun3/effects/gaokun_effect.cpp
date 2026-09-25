/*
 * gaokun_effect.cpp -- AIDL audio effect plugin for the Huawei MateBook E Go.
 *
 * WHAT THIS IS
 * A vendor playback effect that runs the Huawei Histen algorithm on the music
 * stream.  The standard AIDL effects HAL loads it as
 * /vendor/lib64/soundfx/libgaokunhisteneffect.so, named by the
 * <library>/<effect> entries of /vendor/etc/audio_effects_config.xml.
 *
 * ---------------------------------------------------------------------------
 * ★★★ THE ONE ARCHITECTURAL FACT THAT COST A DAY: SUBCLASS EffectImpl.
 *
 * An earlier revision of this file subclassed BnEffect directly and hand-rolled
 * its own FMQs and its own std::thread.  It looked right, it compiled, and it
 * logged:
 *
 *     open: session=98753 ioHandle=13 frames in=4096 out=4096 fmq=8192 floats
 *     command 0 -> state 2            (START, i.e. PROCESSING)
 *     ...and then not a single `meter:` line, ever.
 *
 * Why: the FMQs the framework actually writes into are created by EffectContext,
 * and EffectImpl::open() calls
 *
 *     mImplContext->dupeFmq(ret);      // EffectImpl.cpp -- the LAST step
 *
 * *after* the derived open() has already filled `ret`.  Whatever a hand-rolled
 * open() put in OpenEffectReturn is silently overwritten.  The framework wrote
 * into EffectContext's queues while our worker polled our own, permanently
 * empty, ones.  Nothing logs an error: the framework's waitHalStatusFmq()
 * simply times out, so audio stops dead.
 *
 * The base classes in hardware/interfaces/audio/aidl/default exist precisely to
 * own that machinery, so this file implements only what is genuinely ours:
 *
 *     EffectImpl        owns the FMQ pump, the event flag, the state machine
 *       +-- EffectThread    thread lifecycle + ANDROID_PRIORITY_URGENT_AUDIO
 *       +-- EffectContext   the StatusMQ/DataMQ rings and the work buffer
 *
 * Seven pure virtuals, and nothing else:
 *     getDescriptor / setParameterSpecific / getParameterSpecific /
 *     createContext / releaseContext / getEffectName / effectProcessImpl
 *
 * The three links that make it work:
 *   - EffectContext.cpp, EffectThread.cpp and EffectImpl.cpp are compiled
 *     *into this library* (Android.bp `":effectCommonFile"`).  EffectImpl.cpp
 *     therefore also supplies `destroyEffect` -- this file deliberately does NOT
 *     define one, or the linker would report two.
 *   - EffectImpl::process() is the pump: it waits on the status-FMQ event flag,
 *     takes min(inputMQ->availableToRead(), outputMQ->availableToWrite()), reads
 *     into EffectContext::getWorkBuffer(), calls our effectProcessImpl(), then
 *     writes the result and a status back.  We never touch an FMQ ourselves.
 *   - Every call into us is made with mImplMutex already held, which is what the
 *     REQUIRES(mImplMutex) annotations below assert -- and why the members they
 *     touch are GUARDED_BY it.
 * ---------------------------------------------------------------------------
 *
 * WHY A PLUGIN AND NOT A SERVICE
 * The vendor effects service already running on the device implements IFactory
 * itself and dlopen()s the libraries named in audio_effects_config.xml, looking
 * up three plain C symbols (see EffectFactory.cpp):
 *     createEffect / queryEffect / destroyEffect
 * So there is no binder service to register here, and no
 * AServiceManager_addService() call.
 *
 * ---------------------------------------------------------------------------
 * ★ THE SECOND HALF: SpeakerChain (see speaker_chain.h for the measurements).
 *
 * This file does two jobs that look like one.  Histen is the algorithm; the
 * speaker chain behind it is protection.  They are separate on purpose:
 *
 *     Histen -> LR4 HPF @hpf -> makeup gain -> peak limiter -> soft clip
 *
 * Raising PA volume never matched Windows because the limit was never
 * electrical -- it was diaphragm excursion, spent entirely on sub-150 Hz content
 * that the internal mic measures 42 dB below 1 kHz (i.e. inaudible) while it
 * eats the whole travel budget.  A 150 Hz LR4 high-pass fixed the 150 Hz
 * distortion by 24 dB at *zero* loudness cost at 440 Hz / 1 kHz.  Only after
 * that does more gain make sense -- and it has to go into a limiter rather than
 * into the hard clamp that used to live here, which is what the user heard as
 * "adding gain compensates but distorts obviously".
 * ---------------------------------------------------------------------------
 *
 * FLOAT ONLY
 * EffectImpl::open() rejects anything whose PCM format is not FLOAT_32_BIT, and
 * the framework always hands session effects float -- so there is no int16 path
 * to maintain.
 *
 * WHAT IS NOT NDK-PORTABLE
 * That is why this module is built with Soong inside the ROM tree instead of
 * with the NDK: the audio data path is FMQ, and the NDK does not ship libfmq at
 * all.  It also must be compiled against the platform libc++ (again the Soong
 * default), not libc++_shared, because std::shared_ptr<IEffect> crosses the
 * dlsym() boundary.
 */

#include <aidl/android/hardware/audio/effect/CommandId.h>
#include <aidl/android/hardware/audio/effect/Descriptor.h>
#include <aidl/android/hardware/audio/effect/Flags.h>
#include <aidl/android/hardware/audio/effect/IEffect.h>
#include <aidl/android/hardware/audio/effect/Parameter.h>
#include <aidl/android/media/audio/common/AudioUuid.h>

#include "effect-impl/EffectContext.h"
#include "effect-impl/EffectImpl.h"
#include "histen_chain.h"
#include "speaker_chain.h"

#include <android/log.h>
#include <sys/system_properties.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace aidl::android::hardware::audio::effect {

/* AudioUuid lives in the common AIDL package and is only pulled into this
 * namespace by the audio-effect AIDL headers through qualified names, so spell
 * the using out: AOSP's own effect implementations do the same. */
using ::aidl::android::media::audio::common::AudioUuid;

namespace {

constexpr char kTag[] = "gaokun_effect";

void logi(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, kTag, fmt, ap);
    va_end(ap);
}

/* The TYPE uuid.  Ours is made up, and that is fine: the framework only uses a
 * type uuid for its own special cases (spatializer, haptic generator), and
 * neither is us.  It must match the `type=` attribute of our <effect> entry in
 * audio_effects_config.xml, because EffectConfig::findUuid() honours that
 * attribute only when the effect NAME is absent from its built-in table
 * (loudness_enhancer, equalizer, bassboost, ...).  Our name, `gaokun_histen`, is
 * not in that table, so the attribute is what reaches the framework. */
constexpr char kTypeUuidText[] = "9a1c7f30-4a51-4d9b-8f6e-67616f6b756e";

/* The IMPLEMENTATION uuid -- must match the `uuid=` attribute of the same
 * <effect> entry.  The HAL factory keys its "impl uuid -> library path" map on
 * exactly that value, and it is the value it hands us in createEffect() and
 * queryEffect().  getDescriptor() has to report it back unchanged: the
 * descriptor collection is keyed by (type, uuid), so echoing a different uuid
 * describes an effect nobody asked for, and chain setup then fails with
 *
 *     D AHAL_EffectFactory: createEffect: UUID 9a1c7f31-...
 *     E AHAL_EffectFactory: createEffect: library doesn't exist
 *     E AudioPolicyEffects: addOutputSessionEffects(): failed to create Fx ...
 *
 * We prefer the uuid we were actually asked about (rememberImplUuid()) and use
 * this constant only as the fallback for the case where getDescriptor() runs
 * before any query. */
constexpr char kImplUuidText[] = "b7e4c9a2-3f18-4d6b-9c05-8a1e7f2d4b93";

/* Declared CPU / memory budget, in the units EffectDescriptor.h enforces:
 * cpuLoad counts 0.1% steps, memoryUsage is KiB and must stay under
 * MAX_EFFECTS_MEMORY = 512 -- AudioFlinger refuses the effect with status -38
 * otherwise, logging only
 *     W APM::EffectDescriptor: registerEffect() memory limit exceeded for Fx ...
 * This is a declared budget that is checked before anything is allocated; the
 * real working set is one 480-frame stereo float block plus the Histen instance,
 * which is well under 64 KiB even with the algorithm's 1 MB scratch buffers
 * (those live inside libhw_histen_processing.so's own accounting). */
constexpr int32_t kCpuLoad = 3;
constexpr int32_t kMemoryUsageKiB = 64;

/* ---------------------------------------------------------------------------
 * Runtime knobs, read from system properties so a value can be changed on a
 * running device -- no rebuild, no reboot:
 *
 *     adb shell su -c "setprop persist.gaokun3.histen.makeup 4"   (~1 s, live)
 *     adb shell su -c "setprop persist.gaokun3.histen.scene 3"   (next open())
 *
 * Two refresh classes, and the difference decides whether playback has to be
 * restarted: `enable` and `scene` are read once, at open(), because Histen takes
 * its profile at Init time -- the five speaker knobs (hpf / makeup / limit /
 * ceiling / release) are re-read about once a second and are live.
 *
 * All are strings; an unset or unparsable property means "use the default".
 * ------------------------------------------------------------------------- */

/* Safety valve, read at open() -- i.e. it takes effect on the next playback,
 * never mid-stream.  When it is "0" the chain is never initialised at all, so
 * the effect is a bit-exact wire and no library code runs against our buffers.
 * That matters because a fault inside the Histen library takes down the whole
 * effect service and audioserver with it (music silent, UI sounds seconds
 * late).  Unset means enabled. */
bool histenEnabled() {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("persist.gaokun3.histen.enable", buf) <= 0) return true;
    return !(buf[0] == '0' && buf[1] == 0);
}

/* Which entry of the scene table HistenChain::init() loads.  Read once, at
 * open(): the algorithm takes its profile at Init/SetParams time. */
int readSceneProperty() {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("persist.gaokun3.histen.scene", buf) <= 0) return gaokun::kDefScene;
    const long v = std::strtol(buf, nullptr, 10);
    if (v < 0 || v >= HISTEN_SCENE_COUNT) return gaokun::kDefScene;
    return static_cast<int>(v);
}

/* ---------------------------------------------------------------------------
 * The speaker-protection half -- see speaker_chain.h for the measurements that
 * justify it.  Deliberately independent of `enable`: the high-pass is worth
 * having even with the algorithm switched off, because it is diaphragm
 * excursion (not Histen) that caps how loud this speaker can go.
 *
 *     adb shell su -c "setprop persist.gaokun3.histen.hpf 150"      0 = off
 *     adb shell su -c "setprop persist.gaokun3.histen.makeup 4"     dB
 *     adb shell su -c "setprop persist.gaokun3.histen.limit 1"      0 = limiter off
 *     adb shell su -c "setprop persist.gaokun3.histen.ceiling -1"   dBFS
 *     adb shell su -c "setprop persist.gaokun3.histen.release 120"  ms
 *
 * Unlike enable/scene these are re-read about once a second while audio flows,
 * so they can be auditioned without stopping playback.  (`enable` and `scene`
 * still need a fresh stream: Histen only reads its profile at Init time.)
 * ------------------------------------------------------------------------- */

/* Deployment default for the makeup gain.  SpeakerChain itself defaults to
 * 0 dB -- an honest neutral -- but shipped at 0 dB this device gets *quieter*,
 * because the high-pass removes real energy below 150 Hz.  +4 dB is what the
 * limiter can absorb without working hard: at Histen's measured -5.4 dBFS
 * peaks it applies roughly 4 dB of gain reduction, and at ordinary listening
 * levels it does nothing at all.  0 dB is the "isolate the high-pass" test;
 * past about +9 dB the limiter starts compressing audibly. */
constexpr float kDefaultMakeupDb = 4.0f;

/* Float property with bounds checking.  `fallback` comes back when the property
 * is unset, unparsable, NaN, or outside [lo, hi] -- a stray value must never
 * reach the DSP and turn into a NaN that poisons every sample. */
float readFloatProp(const char* name, float lo, float hi, float fallback) {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, buf) <= 0) return fallback;
    const float v = std::strtof(buf, nullptr);
    if (!(v == v) || v < lo || v > hi) return fallback;
    return v;
}

/* High-pass cutoff.  ⚠ 0 is a *meaningful* value here ("filter off") and it sits
 * below the numeric safety bound, so it is handled before the range check --
 * clamping first would silently turn "off" into 20 Hz. */
float readHpfProp() {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("persist.gaokun3.histen.hpf", buf) <= 0) return gaokun::kDefHpfHz;
    const float v = std::strtof(buf, nullptr);
    if (!(v == v) || v < 0.0f) return gaokun::kDefHpfHz;
    if (v == 0.0f) return 0.0f;
    return v < gaokun::kMinHpfHz ? gaokun::kMinHpfHz
                                 : (v > gaokun::kMaxHpfHz ? gaokun::kMaxHpfHz : v);
}

/* Makeup gain, in dB.
 *
 * ⚠ There is deliberately NO fall-back to the old `persist.gaokun3.histen.gain`
 * property, even though that is the knob the pre-DSP-chain builds used.  It is
 * not worth the compatibility: `gain` is a *stale* property on this device (it
 * was left at 0 by an earlier A/B session), and an alias would let that stale 0
 * silently shadow the +4 dB deployment default -- the chain would install
 * looking correct and quietly deliver 4 dB less than intended.  One source of
 * truth is worth more than script compatibility, and the A/B helper writes
 * `makeup` now anyway. */
float readMakeupProp() {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("persist.gaokun3.histen.makeup", buf) <= 0) return kDefaultMakeupDb;
    const float v = std::strtof(buf, nullptr);
    if (!(v == v) || v < -60.0f || v > gaokun::kMaxMakeupDb) return kDefaultMakeupDb;
    return v;
}

/* Limiter on/off.  Unset means ON: the limiter is protection, not a tone
 * control, so the safe value is the default one. */
bool readLimitProp() {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("persist.gaokun3.histen.limit", buf) <= 0) return true;
    return !(buf[0] == '0' && buf[1] == 0);
}

/* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" -> AudioUuid */
AudioUuid parseUuid(const char* text) {
    unsigned int tl = 0, tm = 0, th = 0, cs = 0;
    unsigned int n[6] = {0};
    AudioUuid u;
    if (std::sscanf(text,
                    "%8x-%4x-%4x-%4x-%2x%2x%2x%2x%2x%2x",
                    &tl, &tm, &th, &cs, &n[0], &n[1], &n[2], &n[3], &n[4], &n[5]) != 10) {
        logi("uuid parse failed for %s", text);
        return u;
    }
    u.timeLow = static_cast<int32_t>(tl);
    u.timeMid = static_cast<int32_t>(tm);
    u.timeHiAndVersion = static_cast<int32_t>(th);
    u.clockSeq = static_cast<int32_t>(cs);
    u.node.assign(n, n + 6);
    return u;
}

/* Diagnostics only.  AOSP does have toString(AudioUuid) overloads in
 * libaudioaidlcommon, but spelling the format out here keeps this file
 * independent of which of them this particular tree exports. */
std::string uuidText(const AudioUuid& u) {
    char buf[40];
    const auto byte = [&u](size_t i) -> unsigned {
        return u.node.size() > i ? static_cast<unsigned>(u.node[i]) & 0xFF : 0u;
    };
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
                  static_cast<uint32_t>(u.timeLow) & 0xFFFFFFFFu,
                  static_cast<uint32_t>(u.timeMid) & 0xFFFFu,
                  static_cast<uint32_t>(u.timeHiAndVersion) & 0xFFFFu,
                  static_cast<uint32_t>(u.clockSeq) & 0xFFFFu, byte(0), byte(1), byte(2),
                  byte(3), byte(4), byte(5));
    return std::string(buf);
}

/* Learned from the factory: the implementation uuid of the slot we occupy.  See
 * the note on kImplUuidText. */
AudioUuid gImplUuid;
bool gImplUuidKnown = false;

void rememberImplUuid(const AudioUuid* uuid) {
    if (uuid == nullptr) return;
    gImplUuid = *uuid;
    gImplUuidKnown = true;
}

AudioUuid implUuid() {
    return gImplUuidKnown ? gImplUuid : parseUuid(kImplUuidText);
}

/* The descriptor we advertise.  Shared by queryEffect() (which the factory uses
 * to build the collection) and getDescriptor() (which the framework asks of the
 * live instance) so the two can never drift apart. */
Descriptor buildDescriptor() {
    Descriptor d{};
    d.common.id.type = parseUuid(kTypeUuidText);
    d.common.id.uuid = implUuid();   /* ★ the slot's uuid, not one of our own */
    d.common.name = "Gaokun Histen";
    d.common.implementor = "gaokun3";
    d.common.cpuLoad = kCpuLoad;
    d.common.memoryUsage = kMemoryUsageKiB;

    /* ★★ INSERT / LAST -- what every AOSP *session* effect declares.
     *
     * LoudnessEnhancerSw::kDescriptor is the reference shape for a
     * post-processing effect living in a playback chain: type INSERT.  Flags.aidl
     * even defaults `type` to INSERT rather than to one of the device types.
     *
     * The type is not cosmetic -- it decides the buffer contract.  INSERT means
     * in-place: the chain's own buffer is what flows through our FMQs, which is
     * exactly what EffectContext sizes.  The device types (PRE_PROC/POST_PROC)
     * belong to the *device effect* path, which this machine does not use.
     *
     * Declaring POST_PROC while sitting in a session chain produced exactly this
     * on 2026-09-23 18:45: effect created, opened, reached PROCESSING, no crash,
     * no framework error -- and then no audio ever reached our FMQs (not one
     * meter line) while the whole device went silent, UI sounds included.
     * Removing the effect from the config brought the sound straight back, which
     * is what pinned it on the type.
     *
     * insert = LAST: we are a post-processing stage and want to sit after any
     * AOSP pre-processing.
     *
     * flags.volume stays NONE: AOSP's LoudnessEnhancer asks for Volume::CTRL,
     * which tells AudioFlinger to stop applying the stream volume and leave it to
     * the effect.  We do not implement volume control, so declaring it would pin
     * playback at full scale.
     *
     * capability is left empty -- AOSP's own LoudnessEnhancerSw does not set it
     * either, and the framework accepts it (this descriptor is already proven on
     * the device: the effect appears in `dumpsys media.audio_flinger` with the
     * right name, flags and uuids). */
    d.common.flags.type = Flags::Type::INSERT;
    d.common.flags.insert = Flags::Insert::LAST;
    return d;
}

}  // namespace

/* ---------------------------------------------------------------------------
 * The effect itself.
 * ------------------------------------------------------------------------- */

class GaokunHisten final : public EffectImpl {
  public:
    static const std::string kEffectName;

    GaokunHisten() { logi("instance created"); }
    ~GaokunHisten() override {
        /* STOP + close, so the worker thread is joined before the members it
         * touches start going away.  EffectImpl's own contract is to be driven
         * through the IEffect interface, but nothing guarantees the framework
         * got that far before dropping its last reference. */
        cleanUp();
        logi("instance destroyed");
    }

    ndk::ScopedAStatus getDescriptor(Descriptor* ret) override;

    ndk::ScopedAStatus setParameterSpecific(const Parameter::Specific& specific)
            REQUIRES(mImplMutex) override;
    ndk::ScopedAStatus getParameterSpecific(const Parameter::Id& id, Parameter::Specific* specific)
            REQUIRES(mImplMutex) override;

    std::shared_ptr<EffectContext> createContext(const Parameter::Common& common)
            REQUIRES(mImplMutex) override;
    RetCode releaseContext() REQUIRES(mImplMutex) override;

    std::string getEffectName() override { return kEffectName; }

    IEffect::Status effectProcessImpl(float* in, float* out, int samples)
            REQUIRES(mImplMutex) override;

  protected:
    /* Only for the journal: the state transitions themselves belong to
     * EffectImpl::command(), which calls this hook at the right moment (before
     * starting the worker, after stopping it). */
    ndk::ScopedAStatus commandImpl(CommandId id) REQUIRES(mImplMutex) override;

  private:
    void meterIn(const float* buf, int samples) REQUIRES(mImplMutex);
    void meterOut(const float* buf, int samples) REQUIRES(mImplMutex);
    void drainMeterWindow() REQUIRES(mImplMutex);
    void refreshSpeakerKnobs() REQUIRES(mImplMutex);

    /* The algorithm.  Not ready (library missing, Init failed, knob turned off)
     * means it is a bit-exact wire -- see HistenChain::process(). */
    gaokun::HistenChain mHisten GUARDED_BY(mImplMutex);

    /* Speaker protection + loudness, after Histen.  This is what replaced the
     * old post-gain hard clamp: LR4 high-pass (frees excursion), makeup gain,
     * peak limiter (owns the ceiling), soft clip (last resort).  See
     * speaker_chain.h -- and the note there about why raising PA alone never
     * worked. */
    gaokun::SpeakerChain mSpeaker GUARDED_BY(mImplMutex);

    /* ---- level meter, flushed every ~2 s ----------------------------------
     * "Is Histen louder or quieter than the wire?" is a question the log has to
     * answer, because by ear a few dB of broadband gain is easy to confuse with
     * a tonal change.  Sums of squares accumulate across a window and come out
     * as dBFS. */
    double mInSum2 GUARDED_BY(mImplMutex) = 0.0;
    double mOutSum2 GUARDED_BY(mImplMutex) = 0.0;
    uint64_t mWinSamples GUARDED_BY(mImplMutex) = 0;
    float mInPeak GUARDED_BY(mImplMutex) = 0.0f;
    float mOutPeak GUARDED_BY(mImplMutex) = 0.0f;
    uint64_t mStarveSeen GUARDED_BY(mImplMutex) = 0;
    uint64_t mSoftClipsSeen GUARDED_BY(mImplMutex) = 0;

    uint32_t mCalls GUARDED_BY(mImplMutex) = 0;
    bool mFirstBuffer GUARDED_BY(mImplMutex) = true;
};

const std::string GaokunHisten::kEffectName = "Gaokun Histen";

ndk::ScopedAStatus GaokunHisten::getDescriptor(Descriptor* ret) {
    if (ret == nullptr) return ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    *ret = buildDescriptor();
    return ndk::ScopedAStatus::ok();
}

/* ---------------------------------------------------------------------------
 * Parameter handling.  This effect is driven entirely by system properties, so
 * there is nothing for an app to set -- but the two hooks still have to exist
 * and, more importantly, must not fail: EffectImpl::setParameter() feeds every
 * non-common tag straight in here, and an error propagates out to a caller that
 * has no idea what to do with it.
 * ------------------------------------------------------------------------- */

ndk::ScopedAStatus GaokunHisten::setParameterSpecific(const Parameter::Specific& specific) {
    /* Tag only, as an integer: the AIDL toString() overloads for these unions are
     * generated per-tree and this file should not depend on which ones exist. */
    logi("setParameterSpecific: tag=%d ignored (property-driven effect)",
         static_cast<int>(specific.getTag()));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus GaokunHisten::getParameterSpecific(const Parameter::Id& id,
                                                      Parameter::Specific* /*specific*/) {
    logi("getParameterSpecific: tag=%d unsupported", static_cast<int>(id.getTag()));
    return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                            "GaokunHistenHasNoParameters");
}

/* ---------------------------------------------------------------------------
 * Context: the base class already does exactly what we want (it creates the
 * StatusMQ/DataMQ rings and sizes them from common.input/output.frameCount).
 * The only reason to override at all is to bring Histen up at the one moment we
 * know the stream exists -- and because a failure there is survivable: the chain
 * stays a wire and the log says why.
 *
 * Called from EffectImpl::open() with mImplMutex held, which is what lets us
 * touch mHisten without a lock of our own.
 * ------------------------------------------------------------------------- */

std::shared_ptr<EffectContext> GaokunHisten::createContext(const Parameter::Common& common) {
    /* Build the DSP chain for the rate we are actually handed rather than for a
     * constant.  This machine's speaker path runs at 48 kHz today, but nothing
     * in the framework promises that, and a biquad built for the wrong rate is
     * a wrong cutoff with no error anywhere.  `base` is AudioConfigBase, which
     * is where sampleRate lives; an unset/zero value falls back to 48 kHz. */
    const int sr = (common.input.base.sampleRate > 0) ? common.input.base.sampleRate : 48000;
    mSpeaker.configure(static_cast<float>(sr));
    refreshSpeakerKnobs();

    logi("speaker chain up: sr=%d %s hpf=%.0fHz makeup=%+.1fdB limit=%d ceiling=%.1fdB "
         "release=%.0fms",
         sr, mSpeaker.tag(), mSpeaker.hpfHz(), mSpeaker.makeupDb(), mSpeaker.limiterOn() ? 1 : 0,
         mSpeaker.ceilingDb(), mSpeaker.releaseMs());

    /* Independent of the speaker chain on purpose: turning Histen off must not
     * also turn off excursion protection. */
    if (!histenEnabled()) {
        logi("Histen disabled by persist.gaokun3.histen.enable=0 -- bit-exact passthrough");
    } else {
        const int scene = readSceneProperty();
        if (mHisten.init(scene)) {
            logi("Histen chain up: scene=%d of %d, block=%d frames", scene, HISTEN_SCENE_COUNT,
                 gaokun::kBlk);
        } else {
            logi("Histen unavailable -- running as a bit-exact passthrough");
        }
    }
    return EffectImpl::createContext(common);
}

RetCode GaokunHisten::releaseContext() {
    mHisten.destroy();
    /* Drop the filter state too: the next open() reconfigures from scratch, and
     * leaving a settled envelope behind would make the first blocks of the new
     * stream duck for no reason. */
    mSpeaker.reset();
    mSoftClipsSeen = 0;
    mCalls = 0;
    mFirstBuffer = true;
    return EffectImpl::releaseContext();
}

/* ---------------------------------------------------------------------------
 * The hook EffectImpl::command() calls around its own state machine.  START
 * arrives before startThread(), STOP/RESET after stopThread(), so nothing needs
 * to be re-armed here -- logging both is enough to prove the chain reached us.
 * ------------------------------------------------------------------------- */

ndk::ScopedAStatus GaokunHisten::commandImpl(CommandId id) {
    logi("command %d (%s)", static_cast<int>(id),
         id == CommandId::START     ? "START"
         : id == CommandId::STOP    ? "STOP"
         : id == CommandId::RESET   ? "RESET"
                                    : "?");
    return EffectImpl::commandImpl(id);
}

/* ---------------------------------------------------------------------------
 * The audio path.
 *
 * EffectImpl::process() calls this with mImplMutex held, with `in` pointing at
 * EffectContext's work buffer -- and, because this is an INSERT effect, with
 * `out` pointing at the *same* buffer.  The contract is:
 *
 *     ★ fmqProduced MUST equal the `samples` we were handed.
 *
 * The framework's EffectHalAidl::waitHalStatusFmq() only checks
 *
 *     status == OK && fmqConsumed == samplesWritten && fmqProduced != 0
 *
 * and then reads min(outputQ->availableToRead(), its own buffer space) samples,
 * so anything we fail to produce leaves the *tail of the block stale*: no error
 * is logged anywhere, the audio is just quietly wrong.  Returning the input
 * count as the output count is therefore not a formality -- and it is safe here
 * because HistenChain::process() has no failure path: on any internal error it
 * copies its input to its output.
 * ------------------------------------------------------------------------- */

IEffect::Status GaokunHisten::effectProcessImpl(float* in, float* out, int samples) {
    IEffect::Status st{};
    /* 0 == STATUS_OK.  Spelled out rather than pulled from a header so this file
     * does not depend on where the constant happens to live in this tree. */
    st.status = 0;
    st.fmqConsumed = samples;
    st.fmqProduced = samples;

    if (samples <= 0) return st;

    if (mFirstBuffer) {
        mFirstBuffer = false;
        logi("first buffer: samples=%d (%d frames), chain=%s", samples, samples / 2,
             mHisten.ready() ? "HISTEN" : "WIRE");
    }

    /* Re-read the DSP knobs a few times a second.  Normally one process() call
     * is one 4096-frame block (~85 ms), so every 16 calls is roughly a second
     * -- which is why hpf/makeup/limit/ceiling/release are audible without
     * stopping playback.  Every setter short-circuits on an unchanged value, so
     * the steady-state cost is five property reads and five comparisons. */
    if ((mCalls++ & 0x0F) == 0) refreshSpeakerKnobs();

    /* Measure BEFORE processing: with in == out the buffer is overwritten in
     * place, so the input level has to be taken first or the comparison is
     * meaningless. */
    meterIn(in, samples);

    /* One call does it all: float -> int32 in the high half-word -> re-block to
     * 480 frames -> Apply -> back to float.  Histen's block size is not a
     * multiple of the framework's (4096), so HistenChain keeps its own rings and
     * a small cushion; the cost is a fixed ~1 block of latency, which is fine
     * because INSERT effects are allowed to be a filter. */
    mHisten.process(in, out, static_cast<size_t>(samples / 2));
    /* An odd float count would leave half a frame behind.  It only matters when
     * in != out; guard anyway so the tail is never uninitialised. */
    if (samples & 1) out[samples - 1] = in[samples - 1];

    /* Excursion first, then loudness: high-pass -> makeup -> limiter -> soft
     * clip.  This is where the old hard clamp used to sit, and the reason the
     * user heard "adding gain compensates but distorts obviously" -- a multiply
     * followed by `if (v > 1) v = 1` clamps peaks flat.  The limiter owns the
     * ceiling now, so loudness can be raised without that. */
    mSpeaker.process(out, samples);

    meterOut(out, samples);
    drainMeterWindow();

    return st;
}

/* Called from the audio thread about once a second -- and once from
 * createContext() so the first block already runs with the right settings.
 * The biquad trig and the exp() for the release coefficient only run when a
 * value actually changes, not once per block. */
void GaokunHisten::refreshSpeakerKnobs() {
    mSpeaker.setHighPass(readHpfProp());
    mSpeaker.setMakeupDb(readMakeupProp());
    mSpeaker.setLimiter(readLimitProp(),
                        readFloatProp("persist.gaokun3.histen.ceiling", gaokun::kMinCeilingDb,
                                      gaokun::kMaxCeilingDb, gaokun::kDefCeilingDb),
                        readFloatProp("persist.gaokun3.histen.release", gaokun::kMinReleaseMs,
                                      gaokun::kMaxReleaseMs, gaokun::kDefReleaseMs));
}

void GaokunHisten::meterIn(const float* buf, int samples) {
    float peak = mInPeak;
    double sum2 = mInSum2;
    for (int i = 0; i < samples; i++) {
        const float v = buf[i];
        sum2 += static_cast<double>(v) * v;
        const float a = std::fabs(v);
        if (a > peak) peak = a;
    }
    mInPeak = peak;
    mInSum2 = sum2;
}

void GaokunHisten::meterOut(const float* buf, int samples) {
    float peak = mOutPeak;
    double sum2 = mOutSum2;
    for (int i = 0; i < samples; i++) {
        const float v = buf[i];
        sum2 += static_cast<double>(v) * v;
        const float a = std::fabs(v);
        if (a > peak) peak = a;
    }
    mOutPeak = peak;
    mOutSum2 = sum2;
    mWinSamples += static_cast<uint64_t>(samples);
}

/* One line per ~2 s of stereo audio.  The `[WIRE]` flag means the chain gave up
 * on Histen and is passing audio through untouched -- the first thing to check
 * when the level moves unexpectedly.
 *
 * Reading the speaker fields:
 *   gr        worst-case gain reduction the limiter applied in this window.  A
 *             positive number means the limiter *is* working; 0 means it never
 *             engaged, which is the normal state for material that is not
 *             already near full scale.
 *   softclip  samples that reached the soft clipper (above -0.5 dBFS).  With the
 *             limiter on this should stay at 0: the limiter holds the ceiling
 *             below the knee, so the clipper is only ever a backstop.  A
 *             nonzero count here means the limiter is off or its ceiling is
 *             above the knee. */
void GaokunHisten::drainMeterWindow() {
    if (mWinSamples < 2ull * 48000 * 2) return;

    const double inDb = 20.0 * std::log10(std::sqrt(mInSum2 / mWinSamples) + 1e-12);
    const double outDb = 20.0 * std::log10(std::sqrt(mOutSum2 / mWinSamples) + 1e-12);
    const uint64_t starved = mHisten.starvedFrames();
    const uint64_t soft = mSpeaker.softClipCount();
    logi("meter: in=%.1f out=%.1f dBFS (%+.1f dB) peak in=%.3f out=%.3f %s hpf=%.0fHz make=%.1fdB "
         "lim=%s ceil=%.1fdB gr=%.1fdB softclip=%llu starve=%llu %s",
         inDb, outDb, outDb - inDb, mInPeak, mOutPeak, mSpeaker.tag(), mSpeaker.hpfHz(),
         mSpeaker.makeupDb(), mSpeaker.limiterOn() ? "on" : "off", mSpeaker.ceilingDb(),
         mSpeaker.maxGainReductionDb(), (unsigned long long)(soft - mSoftClipsSeen),
         (unsigned long long)(starved - mStarveSeen), mHisten.ready() ? "[HISTEN]" : "[WIRE]");
    mStarveSeen = starved;
    mSoftClipsSeen = soft;

    mInSum2 = mOutSum2 = 0.0;
    mWinSamples = 0;
    mInPeak = mOutPeak = 0.0f;
    /* The limiter's worst-case GR is a per-window statistic, unlike its peak
     * counters -- without this the `gr` column would only ever show the all-time
     * maximum and a one-off transient would look permanent. */
    mSpeaker.resetStats();
}

/* The factory entry points.  Kept as thin named-namespace functions so the
 * extern "C" definitions below are one line each and the C names stay exactly
 * what EffectFactory.cpp dlsym()s.  Internal linkage: nothing outside this
 * translation unit should call them. */

namespace {

binder_exception_t factoryCreateEffect(const AudioUuid* in_impl_uuid,
                                       std::shared_ptr<IEffect>* instanceSpp) {
    if (instanceSpp == nullptr) return EX_NULL_POINTER;
    if (in_impl_uuid == nullptr) return EX_ILLEGAL_ARGUMENT;

    /* Deliberately permissive about the uuid.
     *
     * The set of implementation uuids the factory knows is built from
     * audio_effects_config.xml, and it hands us whichever one our <effect> entry
     * declared.  Checking it against kImplUuidText would work today, but the
     * failure mode is bad (a silent "library doesn't exist" from the factory)
     * and the value is not ours to be strict about: the config is the source of
     * truth.  So log a mismatch loudly for the case where the two have drifted,
     * then take the slot we were given and report that uuid back. */
    const AudioUuid expected = parseUuid(kImplUuidText);
    if (*in_impl_uuid != expected) {
        logi("createEffect: impl uuid %s != configured %s -- accepting anyway (config wins)",
             uuidText(*in_impl_uuid).c_str(), kImplUuidText);
    }
    rememberImplUuid(in_impl_uuid);

    *instanceSpp = ndk::SharedRefBase::make<GaokunHisten>();
    logi("createEffect: ok");
    return EX_NONE;
}

binder_exception_t factoryQueryEffect(const AudioUuid* in_impl_uuid, Descriptor* desc) {
    if (desc == nullptr) return EX_NULL_POINTER;
    if (in_impl_uuid == nullptr) return EX_ILLEGAL_ARGUMENT;

    rememberImplUuid(in_impl_uuid);
    *desc = buildDescriptor();
    logi("queryEffect: ok, type=%s impl=%s", kTypeUuidText, uuidText(implUuid()).c_str());
    return EX_NONE;
}

}  // namespace

}  // namespace aidl::android::hardware::audio::effect

/* ---------------------------------------------------------------------------
 * The three symbols EffectFactory.cpp dlsym()s.  C linkage and exact names.
 *
 * Only two are defined here: `destroyEffect` comes from EffectImpl.cpp, which
 * Android.bp compiles into this library via ":effectCommonFile".  Defining it
 * again would be a duplicate symbol at link time.
 * ------------------------------------------------------------------------- */

using aidl::android::hardware::audio::effect::Descriptor;
using aidl::android::hardware::audio::effect::IEffect;
using aidl::android::media::audio::common::AudioUuid;

extern "C" binder_exception_t createEffect(const AudioUuid* in_impl_uuid,
                                           std::shared_ptr<IEffect>* instanceSpp) {
    return aidl::android::hardware::audio::effect::factoryCreateEffect(in_impl_uuid, instanceSpp);
}

extern "C" binder_exception_t queryEffect(const AudioUuid* in_impl_uuid, Descriptor* desc) {
    return aidl::android::hardware::audio::effect::factoryQueryEffect(in_impl_uuid, desc);
}
