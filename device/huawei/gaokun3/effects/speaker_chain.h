/*
 * speaker_chain.h -- the speaker-protection half of the gaokun3 effect.
 *
 * WHY THIS EXISTS
 * ---------------
 * Two independent problems, both measured on this device, both with the same
 * root cause: the speaker's diaphragm excursion is being spent where nobody can
 * hear it.
 *
 * (1) Sub-150 Hz is inaudible but eats the whole excursion budget.
 *     Measured with the internal mic at PA=25 (scripts/audio/gaokun-loudness-v2.sh):
 *         60 Hz   THD -7.9 dB      <-- and only -59 dBFS at the mic,
 *         150 Hz  THD -2.7 dB          i.e. 42 dB below 1 kHz
 *         440 Hz  THD -21.8 dB
 *     The distortion does not move when PA changes, so it is not amplifier
 *     clipping -- it is the driver running out of travel.  Adding an LR4
 *     high-pass at 150 Hz took the 150 Hz distortion from -2.7 to -26.8 dB
 *     while costing 440 Hz / 1 kHz *zero* loudness.  That is how Windows
 *     (Smart PA / Histen APO) gets its loudness, and it is why simply raising
 *     PA never worked: more gain pushes an already-exhausted diaphragm further
 *     past its limit.  Freed excursion is what makes headroom for gain.
 *
 * (2) A post-gain hard clamp is guaranteed to buzz.
 *     An earlier revision raised loudness with a plain multiply followed by
 *     `if (v > 1) v = 1` -- a hard clamp.  Histen's own output already runs
 *     +4.6..+6.3 dB above its input at moderate levels, so any additional
 *     post-gain drove peaks straight into the clamp and the user reported
 *     "adding gain compensates but distorts obviously".  The correct order is
 *     makeup *before* a limiter, so that the limiter, not a clamp, owns the
 *     ceiling.
 *
 * THE CHAIN
 * ---------
 *     Histen -> LR4 HPF @hpf -> makeup gain -> peak limiter -> soft clip
 *               (excursion)     (loudness)     (owns ceiling)   (safety)
 *
 * Placement note: the HPF sits *after* Histen on purpose.  Histen's virtual
 * bass works by generating harmonics (>= 2f), which are all above 150 Hz even
 * when the fundamental is at 60 Hz -- so cutting the inaudible fundamental
 * keeps the harmonics that make the bass *sound* present, while handing the
 * excursion budget back.
 *
 * FAILURE POLICY
 * --------------
 * Same spirit as HistenChain: after configure() nothing allocates, every stage
 * is bounded, and no stage can emit a NaN.  The soft clipper is the last line
 * of defence and is transparent whenever the limiter is doing its job (its
 * knee sits above the limiter ceiling), so it never colours normal audio.
 *
 * Coefficients are recomputed only when a knob actually changes, so the audio
 * thread pays for the trig calls once per change, not once per block.
 */
#ifndef GAOKUN_SPEAKER_CHAIN_H
#define GAOKUN_SPEAKER_CHAIN_H

#include <cmath>
#include <cstdint>

namespace gaokun {

/* LR4 = two cascaded 2nd-order Butterworth sections, so Q = 1/sqrt(2). */
constexpr float kButterworthQ = 0.70710678f;
constexpr float kTwoPi = 6.283185307179586f;

/* Defaults.  Chosen from the measurements above, not by taste:
 *   hpf     150 Hz   -- the value that fixed the 150 Hz distortion outright
 *   ceiling  -1 dBFS -- leaves the limiter just enough room that it never has
 *                       to reach full scale, so the soft clipper stays idle
 *   release 120 ms   -- long enough not to pump on bass, short enough to
 *                       recover before the next transient
 *   makeup    0 dB   -- deliberately neutral at first: the initial state is
 *                       "excursion fixed, level unchanged", so any loudness
 *                       change the listener hears is the HPF at work and not a
 *                       gain knob.  Raise it afterwards, on purpose. */
constexpr float kDefHpfHz = 150.0f;
constexpr float kDefCeilingDb = -1.0f;
constexpr float kDefReleaseMs = 120.0f;
constexpr float kDefMakeupDb = 0.0f;

/* Sanity bounds.  These exist to reject typos, not to limit the user: a stray
 * property must never turn into a NaN that would poison every sample, and a
 * cutoff outside the range where the filter is numerically well behaved at
 * 48 kHz must never reach the biquads. */
constexpr float kMinHpfHz = 20.0f;
constexpr float kMaxHpfHz = 2000.0f;
constexpr float kMinCeilingDb = -24.0f;
constexpr float kMaxCeilingDb = 0.0f;
constexpr float kMinReleaseMs = 5.0f;
constexpr float kMaxReleaseMs = 2000.0f;
constexpr float kMaxMakeupDb = 24.0f;

/* One biquad, direct form I, with its state.  Double precision on purpose: a
 * 4th-order high-pass at 150 Hz runs with a2 very close to 1, and single
 * precision is exactly where that kind of filter starts to drift.  The cost is
 * negligible at 48 kHz stereo. */
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    void reset() { x1 = x2 = y1 = y2 = 0.0; }

    inline double step(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }

    /* RBJ audio-EQ-cookbook high-pass, normalised so that a0 == 1. */
    void setHighPass(double fc, double sr, double q) {
        const double w0 = kTwoPi * fc / sr;
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = (1.0 + cw) / 2.0 / a0;
        b1 = -(1.0 + cw) / a0;
        b2 = b0;
        a1 = -2.0 * cw / a0;
        a2 = (1.0 - alpha) / a0;
    }
};

/* Fourth-order Linkwitz-Riley high-pass: two cascaded Butterworth sections. */
struct Lr4 {
    Biquad a, b;

    void reset() {
        a.reset();
        b.reset();
    }
    inline double step(double x) { return b.step(a.step(x)); }
    void setHighPass(double fc, double sr) {
        a.setHighPass(fc, sr, kButterworthQ);
        b.setHighPass(fc, sr, kButterworthQ);
    }
};

class SpeakerChain {
  public:
    /* Above this the soft clipper starts working.  Must sit above the limiter
     * ceiling (at the -1 dBFS default the ceiling is 0.891 linear) so that in
     * normal operation the clipper is never reached -- it is a last resort,
     * not a tone control. */
    static constexpr float kSoftKnee = 0.94f;

    /* Call once from the non-realtime side (createContext / open).
     *
     * ⚠ Do NOT "simplify" this by seeding the members and then calling the
     * setters: each setter short-circuits when the value is unchanged, so
     * seeding first means the derived state (mMakeupLin / mCeilLin / mRelCoef /
     * the biquad coefficients) is never computed and the object runs on its
     * initialisers.  That exact mistake shipped once -- mCeilLin stayed 1.0f
     * instead of 0.891, so the "limiter" only pulled down to full scale, every
     * block tripped the soft clipper, and the unit test caught it as
     * "peak 0.9779 > ceiling 0.8913, softclips 60586".  Compute everything
     * explicitly here instead. */
    void configure(float sampleRate) {
        mSr = (sampleRate >= 8000.0f && sampleRate <= 384000.0f) ? sampleRate : 48000.0f;

        mHpfHz = kDefHpfHz;
        mLeft.setHighPass(mHpfHz, mSr);
        mRight.setHighPass(mHpfHz, mSr);

        mMakeupDb = kDefMakeupDb;
        mMakeupLin = std::pow(10.0f, mMakeupDb / 20.0f);

        mLimitOn = true;
        mCeilingDb = kDefCeilingDb;
        mCeilLin = std::pow(10.0f, mCeilingDb / 20.0f);
        mReleaseMs = kDefReleaseMs;
        const double relSamples = static_cast<double>(mReleaseMs) * 0.001 * mSr;
        mRelCoef = static_cast<float>(std::exp(-1.0 / (relSamples > 1.0 ? relSamples : 1.0)));

        reset();
    }

    void reset() {
        mLeft.reset();
        mRight.reset();
        mEnv = 0.0f;
        resetStats();
        mSoftClips = 0;
    }

    /* ---- knobs.  All are idempotent and cheap enough to call once per block:
     * a coefficient rebuild happens only when the value actually changes. --- */

    /* fc <= 0 disables the high-pass entirely. */
    void setHighPass(float fc) {
        const float want = (fc <= 0.0f) ? 0.0f : clampf(fc, kMinHpfHz, kMaxHpfHz);
        if (want == mHpfHz) return;
        mHpfHz = want;
        if (want > 0.0f) {
            mLeft.setHighPass(want, mSr);
            mRight.setHighPass(want, mSr);
        } else {
            mLeft.reset();
            mRight.reset();
        }
    }

    void setMakeupDb(float db) {
        const float want = clampf(db, -60.0f, kMaxMakeupDb);
        if (want == mMakeupDb) return;
        mMakeupDb = want;
        mMakeupLin = std::pow(10.0f, want / 20.0f);
    }

    void setLimiter(bool on, float ceilingDb, float releaseMs) {
        const float c = clampf(ceilingDb, kMinCeilingDb, kMaxCeilingDb);
        const float r = clampf(releaseMs, kMinReleaseMs, kMaxReleaseMs);
        if (on == mLimitOn && c == mCeilingDb && r == mReleaseMs) return;
        mLimitOn = on;
        mCeilingDb = c;
        mReleaseMs = r;
        mCeilLin = std::pow(10.0f, c / 20.0f);
        const double samples = static_cast<double>(r) * 0.001 * mSr;
        mRelCoef = static_cast<float>(std::exp(-1.0 / (samples > 1.0 ? samples : 1.0)));
        mEnv = 0.0f;
    }

    /* ---- the audio path.  Interleaved stereo, processed in place. ------ */

    /* `samples` counts FLOATS (= frames * 2), matching what the HAL hands us.
     * An odd trailing float is left untouched -- the caller owns the tail. */
    void process(float* buf, int samples) {
        if (!engaged()) return;   /* bit-exact wire, and no meter overhead */

        const int n = samples & ~1;
        const bool hpf = (mHpfHz > 0.0f);

        for (int i = 0; i < n; i += 2) {
            float l = buf[i];
            float r = buf[i + 1];

            if (hpf) {
                l = static_cast<float>(mLeft.step(l));
                r = static_cast<float>(mRight.step(r));
            }

            if (mMakeupLin != 1.0f) {
                l *= mMakeupLin;
                r *= mMakeupLin;
            }

            if (mLimitOn) {
                /* Linked stereo: one gain for both channels, so the stereo
                 * image never shifts.  Instant attack guarantees the ceiling is
                 * never exceeded; the release is what makes it musical. */
                const float peak = std::fmax(std::fabs(l), std::fabs(r));
                if (peak > mEnv) {
                    mEnv = peak;
                } else {
                    mEnv = mRelCoef * mEnv + (1.0f - mRelCoef) * peak;
                }
                float g = 1.0f;
                if (mEnv > mCeilLin && mEnv > 1e-9f) {
                    g = mCeilLin / mEnv;
                    const double grDb = -20.0 * std::log10(static_cast<double>(g));
                    if (grDb > mGrMaxDb) mGrMaxDb = grDb;
                }
                l *= g;
                r *= g;
            }

            /* Last resort.  Transparent below the knee, asymptotic to +/-1
             * above it -- this is what replaces the hard clamp. */
            const float al = std::fabs(l);
            const float ar = std::fabs(r);
            if (al > kSoftKnee) {
                l = softClip(l, al);
                mSoftClips++;
            }
            if (ar > kSoftKnee) {
                r = softClip(r, ar);
                mSoftClips++;
            }

            buf[i] = l;
            buf[i + 1] = r;

            if (al > mPeakBefore) mPeakBefore = al;
            if (ar > mPeakBefore) mPeakBefore = ar;
            if (std::fabs(l) > mPeakAfter) mPeakAfter = std::fabs(l);
            if (std::fabs(r) > mPeakAfter) mPeakAfter = std::fabs(r);
        }
    }

    /* ---- reporting ---------------------------------------------------- */

    bool hpfOn() const { return mHpfHz > 0.0f; }
    float hpfHz() const { return mHpfHz; }
    float makeupDb() const { return mMakeupDb; }
    bool limiterOn() const { return mLimitOn; }
    float ceilingDb() const { return mLimitOn ? mCeilingDb : 0.0f; }
    float releaseMs() const { return mLimitOn ? mReleaseMs : 0.0f; }

    /* Worst-case gain reduction in dB (a positive number) since the last
     * resetStats().  Zero means the limiter never engaged, which is the
     * expected reading for material that is not already near full scale. */
    double maxGainReductionDb() const { return mGrMaxDb; }
    uint64_t softClipCount() const { return mSoftClips; }
    float peakBefore() const { return mPeakBefore; }
    float peakAfter() const { return mPeakAfter; }

    void resetStats() {
        mGrMaxDb = 0.0;
        mPeakBefore = mPeakAfter = 0.0f;
    }

    /* True when at least one stage is actually doing something. */
    bool engaged() const { return (mHpfHz > 0.0f) || (mMakeupLin != 1.0f) || mLimitOn; }

    /* Short, log-friendly summary of the active configuration. */
    const char* tag() const {
        if (!engaged()) return "SPK:off";
        if (mHpfHz > 0.0f && mLimitOn) return "SPK:hpf+lim";
        if (mHpfHz > 0.0f) return "SPK:hpf";
        if (mLimitOn) return "SPK:lim";
        return "SPK:gain";
    }

  private:
    static float clampf(float v, float lo, float hi) {
        if (!(v == v)) return lo;        /* NaN -> safe default */
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /* `a` is already |x| -- passed in so the caller does not compute it twice. */
    static float softClip(float x, float a) {
        const float s = (x < 0.0f) ? -1.0f : 1.0f;
        if (a <= kSoftKnee) return x;
        const float range = 1.0f - kSoftKnee;
        const float over = a - kSoftKnee;
        const float shaped = kSoftKnee + range * (1.0f - std::exp(-over / range));
        return s * shaped;
    }

    float mSr = 48000.0f;

    float mHpfHz = 0.0f;
    Lr4 mLeft, mRight;          /* POD state, so the chain stays allocation-free */

    float mMakeupDb = kDefMakeupDb;
    float mMakeupLin = 1.0f;

    bool mLimitOn = true;
    float mCeilingDb = kDefCeilingDb;
    float mCeilLin = 1.0f;
    float mReleaseMs = kDefReleaseMs;
    float mRelCoef = 0.0f;
    float mEnv = 0.0f;

    /* stats, reset by the caller once per meter window */
    double mGrMaxDb = 0.0;
    uint64_t mSoftClips = 0;
    float mPeakBefore = 0.0f;
    float mPeakAfter = 0.0f;
};

}  // namespace gaokun

#endif  // GAOKUN_SPEAKER_CHAIN_H
