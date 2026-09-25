/*
 * histen_chain.h -- drive the Huawei Histen core library from inside the AIDL
 * audio effect.
 *
 * The contract below is not guessed.  It was reverse engineered and then
 * verified on this exact device on 2026-09-18
 * (docs/meta/Histen-Android闸门实验结果-2026-09-18.md): after the library was
 * patched for bionic, GetSize / Init / SetParams / Apply all returned 0 and
 * GetSize's magic came back bit-for-bit identical to the Ubuntu run.
 *
 * THE FOUR FUNCTIONS
 *   int ImediaHistenGetSize(void *out8)                       // 8-byte out!
 *   int ImediaHistenInit(unsigned long *handle, void *p2, int sz, void *p3, void *ext)
 *   int ImediaHistenSetParams(void *inner,  void *p2, int sz, void *p3, void *ext)
 *   int ImediaHistenApply(unsigned long *handle, void *p2, int sz, void *cfg)
 *
 * Note the pointer types on Init/Apply versus the value on SetParams -- that
 * asymmetry is real, it is what the two verified call sequences do, and bug 2b
 * below is the whole story of what happens when it is ignored.
 *
 * Three things that are easy to get wrong and cost hours each:
 *
 *  1. GetSize's out parameter is **8 bytes** (a 64-bit magic, not a u32).
 *     Declaring it as `unsigned int *` smashes the neighbouring stack variable.
 *     The high half is the minimum p2 size (82024), the low half is a buffer
 *     size (410272).
 *
 *  2. `*handle` must be pre-set to the work-buffer pointer before Init.
 *     Passing 0 makes Init fail.  So the handle is really "work buffer address"
 *     carried by value through a pointer.
 *
 *  2b. ★★★ Init AND Apply both take the **address of the handle slot**
 *      (`unsigned long *`), while SetParams takes the **dereferenced value**.
 *      Getting this wrong is the single most expensive bug in this port --
 *      it cost a full day and two audioserver crash loops:
 *
 *        ImediaHistenInit     (&hdl, bufB, 82024, p3, ext)   <- address
 *        ImediaHistenSetParams((void *)hdl, bufB, 82024, p3, ext)  <- value
 *        ImediaHistenApply    (&hdl, bufB, 82024, &cfg)      <- address
 *
 *      Apply does `ctx = *(void **)handle` and then loads at `ctx + 0x240`.
 *      Pass the *value* instead and the library reads the first 8 bytes of the
 *      work buffer -- which is NOT a pointer, it is an internal tag/offset
 *      written by Init -- and dereferences it.  Reproduced on this device on
 *      2026-09-24 with an A/B bare-.so probe (scripts/audio/caller_histen_probe2.c,
 *      same library, same buffers, one macro apart):
 *
 *        by address : Apply #1 ret=0, Apply #2 ret=0, output energy 1684228155
 *        by value   : exit=139 (SIGSEGV), fault addr 0x2000012800420,
 *                     pc = ImediaHistenApply+96,
 *                     x19 = handle value, x2 = *(u64 *)work = 0x20000128001e0,
 *                     fault addr == x2 + 0x240
 *
 *      The `x2 + 0x240` relation is the proof: the library loaded the work
 *      buffer's tag word and used it as a context pointer.  Backtracing the
 *      real effect HAL crash to the same offset (ImediaHistenApply+96) and the
 *      same fault address is what closed the case.
 *
 *  3. The cfg struct's frame-length field (offset 16) MUST be 480.  48000 or 0
 *     make Apply return -35, and `fl` must carry the *real* frame count each
 *     call or AlgFade takes the parameter-change path and you hear fades.
 *
 * SAMPLE FORMAT
 *   The core library takes int32 samples whose audio lives in the HIGH
 *   half-word:
 *       in  : ((int32)(clamp(f, -1, 1) * 32767)) << 16
 *       out :  (float)(v >> 16) / 32768
 *   Feeding plain int16 buffers (as the first offline test did) halves the
 *   sample stream and comes out as white noise.  This is the single most
 *   confusing failure mode in the whole port.
 *
 * RE-BLOCKING
 *   Histen only accepts whole 480-frame blocks; AudioFlinger hands us whatever
 *   the mixer produced.  Two ring buffers sit between them.  Same shape as the
 *   verified Ubuntu LADSPA plugin (scripts/audio/histen_ladspa.c), so a bug
 *   here is a bug we have already seen fixed.
 *
 * FAILURE POLICY
 *   Anything that goes wrong makes the chain fall back to bit-exact
 *   passthrough for that buffer.  An audio effect that mangles the speaker or
 *   crashes audioserver is much worse than one that does nothing.
 */
#ifndef GAOKUN_HISTEN_CHAIN_H
#define GAOKUN_HISTEN_CHAIN_H

#include <android/log.h>
#include <dlfcn.h>
#include <sys/system_properties.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "histen_scenes.h"

namespace gaokun {

/* Where the proprietary Histen engine (libhw_histen_processing.so) is looked
 * for, in order.
 *
 * ⚠️ A list of ABSOLUTE paths is the only workable form here. dlopen() consults
 * the linker search path ONLY when the requested name contains no '/', and this
 * engine has already cost one debugging session over exactly that: its own
 * internal dlopen() of a helper used a single baked-in absolute path, so
 * LD_LIBRARY_PATH did not apply to it at all.
 *
 * /vendor first -- that is where a ROM build installs a proprietary blob, and
 *   /vendor/lib64/soundfx is where the AOSP effect libraries themselves live
 *   (libbundlewrapper.so, libdownmix.so, libdynproc.so, ...).
 * /system second -- kept because the out-of-tree deployment this effect was
 *   originally developed against (a KernelSU overlay) installs it there.
 *
 * Finding it in neither place is NOT a fatal condition: init() returns false
 * and the effect degrades to a bit-exact passthrough, so a build that ships
 * without the blob still produces a library that is safe to enable. */
constexpr const char* kHistenLibPaths[] = {
    "/vendor/lib64/soundfx/libhw_histen_processing.so",
    "/system/lib64/soundfx/libhw_histen_processing.so",
};

constexpr int kBlk = 480;             /* Histen's hard frame length (10 ms @48k) */
/* Ring capacity in frames.  The verified Ubuntu LADSPA plugin uses 8192, which
 * was sized for PipeWire quanta; Android instead hands the effect whatever the
 * output thread's block is -- on this device "Normal frame count: 4096" -- so
 * each ring has to swallow a whole 4096-frame block on top of the 480-frame
 * cushion.  16384 leaves a comfortable margin, and an overflow would have to
 * drop frames, which is the one failure mode this file exists to avoid. */
constexpr int kRingFrames = 16384;
/* The size ARGUMENT handed to Init/SetParams/Apply: GetSize()'s high half.
 * ⚠️ This is NOT how much memory to allocate -- see the note in init(). */
constexpr size_t kApplyBufSize = 82024;
/* How much memory to actually give both p2 and the work buffer: 1 MB, matching
 * both verified call sequences.  Never shrink this to kApplyBufSize. */
constexpr size_t kScratchSize = 0x100000;
constexpr int kDefScene = 0;          /* SWS_SPK_LANDSCAPE_ONE                  */
constexpr size_t kP3Bytes = 510;      /* scene p3 payload                       */
constexpr size_t kExtBytes = 32;      /* scene ext payload                      */

/* ---------------------------------------------------------------------------
 * Runtime overrides for the 16-value `ext` block (BEN bass + Vol gain).
 *
 * Why this exists: `ext` used to be a compile-time constant taken straight from
 * the scene table, so the ONLY way to try a different bass setting was to swap
 * the whole scene -- and a scene swap changes 124 of the 255 p3 values at the
 * same time, which makes the result impossible to attribute.  The 2026-09-24
 * A/B proved exactly that: switching to the scene whose BEN gain field is 135
 * made the bass *worse* by 2.7 dB, and there was no way to tell whether BEN was
 * at fault or one of the other 124 changed values was.
 *
 * With these properties each field can be swept on its own, live:
 *
 *     setprop persist.gaokun3.histen.ben.on    1        ext[0]/[1]  switch
 *     setprop persist.gaokun3.histen.ben.freq  300      ext[2]/[3]  Hz
 *     setprop persist.gaokun3.histen.ben.gain  135      ext[4]/[5]  (raw)
 *     setprop persist.gaokun3.histen.ben.thr   15       ext[6]/[7]  (raw)
 *     setprop persist.gaokun3.histen.ben.a     85       ext[8]/[9]  (raw)
 *     setprop persist.gaokun3.histen.ben.b     50       ext[10]/[11] (raw)
 *     setprop persist.gaokun3.histen.vol.dig   60       ext[12]/[13] digital gain
 *     setprop persist.gaokun3.histen.vol.ana   -20      ext[14]/[15] analog gain
 *
 * Unset = keep whatever the scene table says.  These are re-read about once a
 * second while audio flows (same cadence as the speaker-chain knobs), so they
 * can be auditioned without stopping playback.
 *
 * ⚠ Changing them calls SetParams again, and the engine treats that as a
 * parameter change: the next Apply goes through its fade path, so you hear a
 * short cross-fade rather than a jump.  That is expected, not a fault.
 *
 * ⚠ `gain`/`thr`/`a`/`b` are passed through RAW on purpose.  The engine's
 * conversion function for the BEN gain field (0x24e58) is still unreversed, so
 * any scaling we invented would be a guess dressed up as a fact.
 * ------------------------------------------------------------------------- */
constexpr int kExtLen = 16;           /* 16 x u16                               */

/* Layout verified against test_histen3.c: p0 in, p8 out, then
 * sr / fl / ch / u28 / u32. */
struct HistenCfg {
    void *p0;
    void *p8;
    int sr;
    int fl;
    int ch;
    int u28;
    int u32;
};

/* Integer property -> long.  false means "not set", which is how the caller
 * knows to keep the scene-table value instead. */
inline bool readExtProp(const char *name, long &out) {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, buf) <= 0 || buf[0] == '\0') return false;
    char *end = nullptr;
    const long v = std::strtol(buf, &end, 10);
    if (end == buf) return false;
    out = v;
    return true;
}

/* ext slots are u16, but the analog-gain pair is negative in every scene table
 * (-20 / -40), so accept signed input and store the two's-complement bits. */
inline uint16_t extU16(long v) {
    if (v < -32768) v = -32768;
    if (v > 65535) v = 65535;
    return static_cast<uint16_t>(v & 0xFFFFL);
}

/* ---------------------------------------------------------------------------
 * Runtime overrides for the p3 EQ block.
 *
 * Located by cross-referencing two things (see
 * android/docs/实测-ext块无效-2026-09-24.md for how ext was ruled out):
 *
 *   1. `ImediaSwsSetGeq` (0xf960) reads p3+92/94 (idx46/47) as a header and then
 *      loops 12 times over `x24 = p3 + 0x60` (idx48), reading BOTH [x24] and
 *      [x24 + 22] -- i.e. two parallel arrays 11 indices apart.
 *   2. Comparing scenes: LANDSCAPE_ONE / TWO / THREE / PORTRAIT_ONE differ in
 *      only **16 of the 255** p3 values, and all 16 sit in idx96..111 -- a
 *      contiguous run.  In ONE they are the textbook octave centres
 *      31/62/125/250/500/1k/2k/4k/8k/16k Hz; TWO replaces them with an
 *      irregular set (65/120/560/2200/...) that looks device-specific.
 *   3. The *_MOVIE scenes differ from the base scenes only in idx47, idx57 and
 *      idx60..70 -- and every value in idx60..70 is a "doubled byte" pair
 *      (0xb4b4, 0x3232, 0x8c8c...), i.e. one value packed for L and R.
 *      MOVIE uses consistently LARGER values there.
 *
 * So idx60..70 is the working EQ gain array.  Each property writes one slot,
 * packed into both halves:
 *
 *     setprop persist.gaokun3.histen.eq.0  180     # -> 0xb4b4 (L=R=180)
 *     ...through eq.10 (11 slots: idx60..idx70)
 *
 * Unset = keep the scene value.  Re-read about once a second, same as ext.
 *
 * ⚠ Raw passthrough again, and for a sharper reason than with ext: the value
 * looks like 0.1 dB (ONE's idx61 = 0xb4 = 180 -> -7.6 dB if signed, +18.0 if
 * unsigned) and BOTH readings are plausible.  Guessing would bake a wrong unit
 * into every number we publish, so the sweep has to tell us the sign convention.
 * ------------------------------------------------------------------------- */
constexpr int kEqBase = 60;    /* idx60                                      */
constexpr int kEqLen = 11;     /* idx60..70                                  */

/* Scene p3 first, then any eq.N that is set.  Each slot is written to BOTH
 * halves so L and R stay identical -- the scene tables do the same. */
inline void buildP3(uint16_t *out, const uint16_t *base) {
    std::memcpy(out, base, kP3Bytes);
    char name[64];
    long v;
    for (int i = 0; i < kEqLen; i++) {
        std::snprintf(name, sizeof(name), "persist.gaokun3.histen.eq.%d", i);
        if (!readExtProp(name, v)) continue;
        if (v < -128) v = -128;
        if (v > 255) v = 255;
        const uint16_t b = static_cast<uint16_t>(v & 0xFF);
        out[kEqBase + i] = static_cast<uint16_t>((b << 8) | b);
    }
}

/* Scene value first, then every property that is actually set. */
inline void buildExt(uint16_t *out, const uint16_t *base) {
    std::memcpy(out, base, kExtBytes);
    long v;
    if (readExtProp("persist.gaokun3.histen.ben.on", v))  out[0]  = out[1]  = v ? 1u : 0u;
    if (readExtProp("persist.gaokun3.histen.ben.freq", v)) out[2] = out[3]  = extU16(v);
    if (readExtProp("persist.gaokun3.histen.ben.gain", v)) out[4] = out[5]  = extU16(v);
    if (readExtProp("persist.gaokun3.histen.ben.thr", v))  out[6] = out[7]  = extU16(v);
    if (readExtProp("persist.gaokun3.histen.ben.a", v))    out[8] = out[9]  = extU16(v);
    if (readExtProp("persist.gaokun3.histen.ben.b", v))    out[10] = out[11] = extU16(v);
    if (readExtProp("persist.gaokun3.histen.vol.dig", v))  out[12] = out[13] = extU16(v);
    if (readExtProp("persist.gaokun3.histen.vol.ana", v))  out[14] = out[15] = extU16(v);
}

class HistenChain {
  public:
    ~HistenChain() { destroy(); }

    /* Returns false if anything at all goes wrong -- the caller then keeps
     * passing audio through untouched. */
    bool init(int sceneIndex = kDefScene) {
        destroy();

        for (const char* path : kHistenLibPaths) {
            mHandle = dlopen(path, RTLD_NOW);
            if (mHandle != nullptr) {
                log("Histen engine loaded from %s", path);
                break;
            }
            log("dlopen %s failed: %s", path, dlerror());
        }
        if (mHandle == nullptr) {
            log("Histen engine not found in any known location -- passthrough");
            return false;
        }
        mGetSize = reinterpret_cast<FnGetSize>(dlsym(mHandle, "ImediaHistenGetSize"));
        mInit = reinterpret_cast<FnInit>(dlsym(mHandle, "ImediaHistenInit"));
        mSetParams = reinterpret_cast<FnSetParams>(dlsym(mHandle, "ImediaHistenSetParams"));
        mApply = reinterpret_cast<FnApply>(dlsym(mHandle, "ImediaHistenApply"));
        if (!mGetSize || !mInit || !mSetParams || !mApply) {
            log("dlsym failed (GetSize=%p Init=%p SetParams=%p Apply=%p)", mGetSize,
                mInit, mSetParams, mApply);
            destroy();
            return false;
        }

        /* ★ 8-byte out parameter, and 8-byte aligned. */
        uint64_t magic __attribute__((aligned(8))) = 0;
        if (mGetSize(&magic) != 0) {
            log("GetSize failed");
            destroy();
            return false;
        }

        /* ★★ Both buffers are 1 MB -- exactly like BOTH verified call sequences
         * (scripts/audio/caller_histen_probe.c has
         *     static unsigned char work[0x100000], bufB[0x100000];
         * and _histen_re/test_histen3.c does `calloc(1, 0x100000)` for both),
         * even though the size ARGUMENT passed to Init/SetParams/Apply is 82024.
         *
         * ★ The allocated size and the declared size are NOT the same thing for
         * this library.  GetSize()'s 64-bit magic says as much:
         *     0x00014068_000642a0
         *     high half 0x14068 =  82024  <- minimum accepted by Init
         *     low  half 0x642a0 = 410272  <- the real working set
         * The Ubuntu LADSPA plugin hands it exactly 82024 bytes and has never
         * misbehaved, so the minimum is probably enough on glibc; but there is
         * no reason to gamble on bionic, and 1 MB is what the two recipes that
         * are known to work on THIS device both use.
         *
         * ⚠ History note: the crash at ImediaHistenApply+96 that was once blamed
         * on a shrunken p2 was actually bug 2b above (Apply's first argument).
         * p2 was grown back to 1 MB at the same time and the crash persisted,
         * which is what eventually ruled this theory out.  Keep 1 MB -- it is
         * the verified value -- but do not expect it to fix anything by itself.
         * Do NOT "optimise" this number either way. */
        mP2 = calloc(1, kScratchSize);
        mWork = calloc(1, kScratchSize);
        mP3 = calloc(1, 0x2000);
        mSub = calloc(1, 0x800);
        mP4 = calloc(1, 0x100);
        mInRing = static_cast<int32_t *>(calloc(kRingFrames * 2, sizeof(int32_t)));
        mOutRing = static_cast<int32_t *>(calloc(kRingFrames * 2, sizeof(int32_t)));
        if (!mP2 || !mWork || !mP3 || !mSub || !mP4 || !mInRing || !mOutRing) {
            log("allocation failed");
            destroy();
            return false;
        }
        mInBuf = static_cast<int32_t *>(calloc(kBlk * 2, sizeof(int32_t)));
        mOutBuf = static_cast<int32_t *>(calloc(kBlk * 2, sizeof(int32_t)));
        if (!mInBuf || !mOutBuf) {
            log("block allocation failed");
            destroy();
            return false;
        }

        if (sceneIndex < 0 || sceneIndex >= HISTEN_SCENE_COUNT) sceneIndex = kDefScene;
        mSceneIndex = sceneIndex;   /* refreshExt() needs it to re-read the base */
        const HistenScene &sc = histen_scenes[sceneIndex];

        /* ★ The handle starts out as the work-buffer address, passed by pointer. */
        unsigned long hdl = reinterpret_cast<unsigned long>(mWork);
        memcpy(mP3, sc.p3, kP3Bytes);
        /* Both blocks go through their builder so a setprop made before playback
         * takes effect on the very first buffer, instead of waiting for the
         * ~1 s refresh to notice it.  buildP3() only touches idx60..70, which is
         * well below the 0x200 pointer block written just below. */
        buildP3(reinterpret_cast<uint16_t *>(mP3), sc.p3);
        buildExt(mExt, sc.ext);
        memcpy(mP4, mExt, kExtBytes);
        /* Four pointers to the shared sub-buffer, offsets verified on hardware. */
        *reinterpret_cast<unsigned long *>(static_cast<char *>(mP3) + 0x200) =
                reinterpret_cast<unsigned long>(mSub);
        *reinterpret_cast<unsigned long *>(static_cast<char *>(mP3) + 0x208) =
                reinterpret_cast<unsigned long>(mSub);
        *reinterpret_cast<unsigned long *>(static_cast<char *>(mP3) + 0x210) =
                reinterpret_cast<unsigned long>(mSub);
        *reinterpret_cast<unsigned long *>(static_cast<char *>(mP3) + 0x218) =
                reinterpret_cast<unsigned long>(mSub);

        if (mInit(&hdl, mP2, kApplyBufSize, mP3, mP4) != 0) {
            log("Init failed (scene %s)", sc.name);
            destroy();
            return false;
        }
        if (mSetParams(reinterpret_cast<void *>(hdl), mP2, kApplyBufSize, mP3, mP4) != 0) {
            log("SetParams failed (scene %s)", sc.name);
            destroy();
            return false;
        }
        mHdl = hdl;

        /* ★ offset 16 (sr) MUST be 480, not the 48000 sample rate. */
        memset(&mCfg, 0, sizeof(mCfg));
        mCfg.sr = kBlk;
        mCfg.fl = kBlk;
        mCfg.ch = 2;
        mCfg.u32 = 2;

        /* Warm-up pass, same as the verified sequence: Apply once with a whole
         * block so the algorithm builds its internal state before real audio.
         *
         * ★ The log line just above it is deliberate: this call is the first one
         * that runs library code with our buffers, and on 2026-09-23 it was the
         * exact instruction that SIGSEGV'd (ImediaHistenApply+96) and took
         * audioserver down.  If that ever happens again, having "warming up" in
         * the log proves the crash is here rather than in dlopen/Init/SetParams.
         * (The 2026-09-23/24 crash itself turned out to be bug 2b at the top of
         * this file -- the first argument -- and is fixed by passing &mHdl.) */
        log("init: dlopen+GetSize(0x%llx)+Init+SetParams ok, warming up "
            "(p2=%p work=%p hdl=%p &hdl=%p p3=%p p4=%p)",
            (unsigned long long)magic, mP2, mWork, reinterpret_cast<void *>(mHdl),
            static_cast<void *>(&mHdl), mP3, mP4);
        memset(mInBuf, 0, kBlk * 2 * sizeof(int32_t));
        memset(mOutBuf, 0, kBlk * 2 * sizeof(int32_t));
        mCfg.p0 = mInBuf;
        mCfg.p8 = mOutBuf;
        /* ★ Apply takes the ADDRESS of the handle slot (see bug 2b) -- NOT mHdl.
         * Passing the value dereferences the work buffer's tag word and dies at
         * ImediaHistenApply+96 with fault addr 0x2000012800420. */
        mApply(static_cast<void *>(&mHdl), mP2, kApplyBufSize, &mCfg);

        /* ★ Prime the output ring with one block, exactly as the LADSPA plugin
         * does (`h->rout_n = BLK`).  Without that cushion the pump can be asked
         * for a block it cannot fill yet: Android's block is 4096 frames, and
         * 4096 is not a multiple of 480, so a few frames would come up short at
         * every block boundary and be zero-filled -- i.e. silently dropped.
         * Cost: a constant 480 frames (10 ms) of extra latency. */
        mInHead = mInCount = 0;
        mOutHead = 0;
        mOutCount = 0;
        ringPush(mOutRing, mOutHead, mOutCount, mOutBuf, kBlk);

        mStarved = 0;
        mReady = true;
        log("ready: scene %s, p2=%p, primed %d frames", sc.name, mP2, mOutCount);
        logEq("init");
        logExt("init");
        return true;
    }

    /* Re-reads the ext overrides and, if any of them changed, pushes the new
     * block through SetParams.  Called from process() about once a second.
     *
     * ⚠ The engine sees a SetParams as a parameter change and fades on the next
     * Apply, so every knob move costs one short cross-fade.  Compare-then-set
     * keeps that from happening continuously. */
    bool refreshExt() {
        if (!mReady || mSetParams == nullptr) return false;

        const HistenScene &sc = histen_scenes[mSceneIndex];
        uint16_t nextExt[kExtLen];
        uint16_t nextP3[kP3Bytes / 2];
        buildP3(nextP3, sc.p3);
        buildExt(nextExt, sc.ext);

        uint16_t *curP3 = reinterpret_cast<uint16_t *>(mP3);
        const bool p3Changed = std::memcmp(nextP3, curP3, kP3Bytes) != 0;
        const bool extChanged = std::memcmp(nextExt, mExt, kExtBytes) != 0;
        if (!p3Changed && !extChanged) return false;

        /* Remember what the engine currently holds, so a failure can be undone.
         * mP3's pointer block (0x200..0x218) must be preserved verbatim -- that
         * is why buildP3() works from the scene table rather than from mP3. */
        const int slots = kP3Bytes / 2;
        for (int i = 0; i < slots; i++) {
            if (i >= kEqBase && i < kEqBase + kEqLen) curP3[i] = nextP3[i];
        }
        std::memcpy(mExt, nextExt, kExtBytes);
        std::memcpy(mP4, mExt, kExtBytes);

        if (mSetParams(reinterpret_cast<void *>(mHdl), mP2, kApplyBufSize, mP3, mP4) != 0) {
            log("SetParams failed on refresh -- keeping previous values");
            buildP3(curP3, sc.p3);   /* scene values, i.e. the last good state */
            buildExt(mExt, sc.ext);
            std::memcpy(mP4, mExt, kExtBytes);
            return false;
        }
        logEq("refresh");
        logExt("refresh");
        return true;
    }

    /* One line per EQ refresh so the A/B log shows which slot moved. */
    void logEq(const char *why) {
        uint16_t *p3 = reinterpret_cast<uint16_t *>(mP3);
        char buf[160];
        int n = 0;
        for (int i = 0; i < kEqLen; i++) {
            n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n),
                               " %d:%d", kEqBase + i,
                               static_cast<int8_t>(p3[kEqBase + i] & 0xFF));
        }
        log("eq[%s]:%s", why, buf);
    }

    void destroy() {
        mReady = false;
        if (mHandle) {
            dlclose(mHandle);
            mHandle = nullptr;
        }
        free(mP2); free(mWork); free(mP3); free(mSub); free(mP4);
        free(mInRing); free(mOutRing); free(mInBuf); free(mOutBuf);
        mP2 = mWork = mP3 = mSub = mP4 = nullptr;
        mInRing = mOutRing = mInBuf = mOutBuf = nullptr;
        mGetSize = nullptr; mInit = nullptr; mSetParams = nullptr; mApply = nullptr;
        mHdl = 0;
    }

    bool ready() const { return mReady; }

    /* Cumulative frames that had to be replaced by silence because the
     * algorithm had not produced them in time.  Anything above zero here is
     * audible as a dropout, so the worker logs the delta. */
    uint64_t starvedFrames() const { return mStarved; }

    /* in/out: interleaved stereo float, `frames` frames.  Never fails: on any
     * error the input is copied to the output. */
    void process(const float *in, float *out, size_t frames) {
        if (!mReady) {
            memcpy(out, in, frames * 2 * sizeof(float));
            return;
        }

        /* ext overrides.  AudioFlinger hands ~4096-frame blocks at 48 kHz, i.e.
         * about 12 process() calls per second, so &0x0F lands near once a second
         * -- the same cadence as the speaker-chain knobs. */
        if ((mExtTick++ & 0x0F) == 0) refreshExt();

        int32_t tmp[2048 * 2];
        size_t done = 0;
        while (done < frames) {
            size_t chunk = frames - done;
            if (chunk > 2048) chunk = 2048;
            for (size_t i = 0; i < chunk; i++) {
                float l = in[(done + i) * 2];
                float r = in[(done + i) * 2 + 1];
                l *= 32767.0f;
                r *= 32767.0f;
                if (l > 32767.0f) l = 32767.0f; else if (l < -32768.0f) l = -32768.0f;
                if (r > 32767.0f) r = 32767.0f; else if (r < -32768.0f) r = -32768.0f;
                /* audio in the HIGH half-word -- see the note at the top */
                tmp[i * 2] = static_cast<int32_t>(l) << 16;
                tmp[i * 2 + 1] = static_cast<int32_t>(r) << 16;
            }
            ringPush(mInRing, mInHead, mInCount, tmp, static_cast<int>(chunk));
            done += chunk;
        }

        mCfg.p0 = mInBuf;
        mCfg.p8 = mOutBuf;
        mCfg.fl = kBlk;
        while (mInCount >= kBlk) {
            ringTake(mInRing, mInHead, mInCount, mInBuf, kBlk);
            /* ★ &mHdl, not mHdl -- see bug 2b at the top of this file. */
            if (mApply(static_cast<void *>(&mHdl), mP2, kApplyBufSize, &mCfg) != 0) {
                /* algorithm died: fall back to a wire for this block */
                memcpy(mOutBuf, mInBuf, kBlk * 2 * sizeof(int32_t));
                mFailed++;
                if (mFailed > 64) {
                    log("Apply kept failing (%d) -- going to passthrough", mFailed);
                    mReady = false;
                    memcpy(out, in, frames * 2 * sizeof(float));
                    return;
                }
            }
            ringPush(mOutRing, mOutHead, mOutCount, mOutBuf, kBlk);
        }

        size_t avail = static_cast<size_t>(mOutCount);
        size_t take = frames < avail ? frames : avail;
        size_t outI = 0;
        while (outI < take) {
            size_t chunk = take - outI;
            if (chunk > 2048) chunk = 2048;
            ringPop(mOutRing, mOutHead, mOutCount, tmp, static_cast<int>(chunk));
            for (size_t i = 0; i < chunk; i++) {
                out[(outI + i) * 2] = static_cast<float>(tmp[i * 2] >> 16) / 32768.0f;
                out[(outI + i) * 2 + 1] = static_cast<float>(tmp[i * 2 + 1] >> 16) / 32768.0f;
            }
            outI += chunk;
        }
        /* Should never trigger once the ring is primed and the pump keeps
         * consumed == produced.  If it ever does, starvedFrames() says how much
         * audio was replaced by silence. */
        for (size_t i = take; i < frames; i++) {
            out[i * 2] = 0.0f;
            out[i * 2 + 1] = 0.0f;
        }
        mStarved += frames - take;
    }

  private:
    using FnGetSize = int (*)(void *);
    using FnInit = int (*)(void *, void *, int, void *, void *);
    using FnSetParams = int (*)(void *, void *, int, void *, void *);
    using FnApply = int (*)(void *, void *, int, void *);

    /* One line per ext refresh so the A/B log shows exactly which field moved.
     * Written with the same tag as everything else in this file. */
    void logExt(const char *why) {
        log("ext[%s]: on=%u freq=%u gain=%u thr=%u a=%u b=%u volDig=%u/%u volAna=%d/%d",
            why, mExt[0], mExt[2], mExt[4], mExt[6], mExt[8], mExt[10],
            mExt[12], mExt[13],
            static_cast<int16_t>(mExt[14]), static_cast<int16_t>(mExt[15]));
    }

    /* One tag for the whole plugin so `logcat -s gaokun_effect` shows the
     * library contract and the algorithm state next to each other. */
    static void log(const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        __android_log_vprint(ANDROID_LOG_INFO, "gaokun_effect", fmt, ap);
        va_end(ap);
    }

    static void ringPush(int32_t *ring, int &head, int &count, const int32_t *src, int frames) {
        for (int i = 0; i < frames; i++) {
            int idx = ((head + count) % kRingFrames) * 2;
            ring[idx] = src[i * 2];
            ring[idx + 1] = src[i * 2 + 1];
            if (count < kRingFrames) {
                count++;
            } else {
                head = (head + 1) % kRingFrames; /* drop oldest */
            }
        }
    }
    static void ringPop(int32_t *ring, int &head, int &count, int32_t *dst, int frames) {
        for (int i = 0; i < frames && count > 0; i++) {
            dst[i * 2] = ring[head * 2];
            dst[i * 2 + 1] = ring[head * 2 + 1];
            head = (head + 1) % kRingFrames;
            count--;
        }
    }
    static void ringTake(int32_t *ring, int &head, int &count, int32_t *dst, int frames) {
        ringPop(ring, head, count, dst, frames);
    }

    void *mHandle = nullptr;
    FnGetSize mGetSize = nullptr;
    FnInit mInit = nullptr;
    FnSetParams mSetParams = nullptr;
    FnApply mApply = nullptr;

    void *mP2 = nullptr;
    void *mWork = nullptr;
    void *mP3 = nullptr;
    void *mSub = nullptr;
    void *mP4 = nullptr;
    unsigned long mHdl = 0;

    int32_t *mInRing = nullptr;
    int32_t *mOutRing = nullptr;
    int32_t *mInBuf = nullptr;
    int32_t *mOutBuf = nullptr;
    int mInHead = 0, mInCount = 0, mOutHead = 0, mOutCount = 0;

    HistenCfg mCfg{};
    bool mReady = false;
    int mFailed = 0;
    uint64_t mStarved = 0;   /* frames zero-filled because output was not ready */

    /* ext overrides -- see the block above buildExt() for the property names. */
    uint16_t mExt[kExtLen] = {0};
    int mSceneIndex = kDefScene;
    unsigned mExtTick = 0;
};

}  // namespace gaokun

#endif  // GAOKUN_HISTEN_CHAIN_H
