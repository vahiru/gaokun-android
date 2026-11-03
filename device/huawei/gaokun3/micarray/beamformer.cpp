#include "beamformer.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace gaokun::micarray {

namespace {

// The coefficient file: 24 planes of 1025 float32 (DC to Nyquist), then other data, then the XOR
// of every 32-bit word before it (seeded with 0x55555555), twice.
constexpr size_t kFileBytes = 347592;
constexpr size_t kFileBins = kBins + 1;
constexpr size_t kChecksumWords = kFileBytes / 4 - 2;
constexpr uint32_t kChecksumSeed = 0x55555555u;

uint32_t le32(const uint8_t* p) {
    return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24;
}

// Analysis window sin(pi (n + 1) / (N + 1)); the synthesis window divides it by the sum of the
// squared windows that overlap at n, so that analysis and synthesis together add up to one.
struct Windows {
    std::array<float, kWindow> analysis;
    std::array<float, kWindow> synthesis;
    Windows() {
        double a[kWindow];
        for (size_t n = 0; n < kWindow; ++n) a[n] = std::sin(M_PI * (n + 1) / (kWindow + 1));
        for (size_t n = 0; n < kWindow; ++n) {
            double sum = 0;
            for (size_t k = n % kHop; k < kWindow; k += kHop) sum += a[k] * a[k];
            analysis[n] = static_cast<float>(a[n]);
            synthesis[n] = static_cast<float>(a[n] / sum);
        }
    }
};

// In float, as the implementation the weights were chosen with: the output matches it bit for bit.
struct Twiddles {
    std::array<std::complex<float>, kFftSize / 2> w;
    Twiddles() {
        for (size_t i = 0; i < w.size(); ++i) {
            const float angle = -2.0f * static_cast<float>(M_PI) * static_cast<float>(i) / kFftSize;
            w[i] = {std::cos(angle), std::sin(angle)};
        }
    }
};

const Windows& windows() {
    static const Windows w;
    return w;
}

const Twiddles& twiddles() {
    static const Twiddles t;
    return t;
}

// In-place radix-2 FFT, unscaled in both directions.
void fft(std::array<std::complex<float>, kFftSize>& x, bool inverse) {
    for (size_t i = 1, j = 0; i < kFftSize; ++i) {
        size_t bit = kFftSize >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    const auto& w = twiddles().w;
    for (size_t len = 2; len <= kFftSize; len <<= 1) {
        const size_t half = len >> 1, stride = kFftSize / len;
        for (size_t block = 0; block < kFftSize; block += len) {
            for (size_t i = 0; i < half; ++i) {
                const std::complex<float> t = inverse ? std::conj(w[i * stride]) : w[i * stride];
                const std::complex<float> even = x[block + i];
                const std::complex<float> odd = x[block + i + half] * t;
                x[block + i] = even + odd;
                x[block + i + half] = even - odd;
            }
        }
    }
}

int16_t toS16(float v) {
    if (!std::isfinite(v)) return 0;
    const float r = v < 0 ? v - 0.5f : v + 0.5f;
    if (r >= std::numeric_limits<int16_t>::max()) return std::numeric_limits<int16_t>::max();
    if (r <= std::numeric_limits<int16_t>::min()) return std::numeric_limits<int16_t>::min();
    return static_cast<int16_t>(r);
}

}  // namespace

bool loadWeights(const char* path, Weights* weights, std::string* error) {
    std::unique_ptr<FILE, int (*)(FILE*)> f(std::fopen(path, "rbe"), std::fclose);
    if (!f) {
        *error = std::string("cannot open: ") + std::strerror(errno);
        return false;
    }
    std::vector<uint8_t> bytes(kFileBytes + 1);
    if (std::fread(bytes.data(), 1, bytes.size(), f.get()) != kFileBytes) {
        *error = "not " + std::to_string(kFileBytes) + " bytes long";
        return false;
    }
    uint32_t sum = kChecksumSeed;
    for (size_t i = 0; i < kChecksumWords; ++i) sum ^= le32(&bytes[i * 4]);
    if (le32(&bytes[kChecksumWords * 4]) != sum || le32(&bytes[kChecksumWords * 4 + 4]) != sum) {
        *error = "checksum mismatch";
        return false;
    }
    // The first bank: output o, input i has its real part in plane 6 o + 2 i, its imaginary part
    // in the plane after it.
    for (size_t o = 0; o < kOutputs; ++o) {
        for (size_t i = 0; i < kInputs; ++i) {
            const uint8_t* re = &bytes[(6 * o + 2 * i) * kFileBins * 4];
            const uint8_t* im = re + kFileBins * 4;
            for (size_t b = 0; b < kBins; ++b) {
                float r, m;
                const uint32_t rb = le32(re + b * 4), mb = le32(im + b * 4);
                std::memcpy(&r, &rb, 4);
                std::memcpy(&m, &mb, 4);
                if (!std::isfinite(r) || !std::isfinite(m)) {
                    *error = "non-finite weight";
                    return false;
                }
                weights->w[o][i][b] = {r, m};
            }
        }
    }
    return true;
}

Beamformer::Beamformer(const Weights& weights) : mWeights(weights) {
    windows();
    twiddles();
}

void Beamformer::reset() {
    mFill = 0;
    for (auto& c : mIn) c.fill(0);
    for (auto& c : mOut) c.fill(0);
    for (auto& c : mHistory) c.fill(0);
    for (auto& c : mOverlap) c.fill(0);
}

void Beamformer::process(const int16_t* in, size_t channels,
                         const std::array<size_t, kInputs>& map, int16_t* out, size_t frames) {
    for (size_t f = 0; f < frames; ++f) {
        for (size_t i = 0; i < kInputs; ++i) mIn[i][mFill] = in[f * channels + map[i]];
        for (size_t o = 0; o < kOutputs; ++o) out[f * kOutputs + o] = mOut[o][mFill];
        if (++mFill == kHop) {
            processHop(mIn, mOut);
            mFill = 0;
        }
    }
}

void Beamformer::processHop(const HopIn& in, HopOut& out) {
    const Windows& win = windows();
    for (size_t i = 0; i < kInputs; ++i) {
        auto& h = mHistory[i];
        std::memmove(h.data(), h.data() + kHop, (kWindow - kHop) * sizeof(float));
        for (size_t n = 0; n < kHop; ++n) h[kWindow - kHop + n] = in[i][n];
        mFft.fill({});
        for (size_t n = 0; n < kWindow; ++n) mFft[n] = {h[n] * win.analysis[n], 0};
        fft(mFft, false);
        std::copy(mFft.begin(), mFft.begin() + kBins, mSpectra[i].begin());
    }
    for (size_t o = 0; o < kOutputs; ++o) {
        mFft.fill({});
        for (size_t b = 0; b < kBins; ++b) {
            std::complex<float> y = 0;
            for (size_t i = 0; i < kInputs; ++i) y += mSpectra[i][b] * mWeights.w[o][i][b];
            if (b == 0) {
                mFft[0] = {y.real(), 0};
            } else {
                mFft[b] = y;
                mFft[kFftSize - b] = std::conj(y);
            }
        }
        fft(mFft, true);
        auto& ov = mOverlap[o];
        for (size_t n = 0; n < kWindow; ++n) {
            ov[n] += mFft[n].real() * (1.0f / kFftSize) * win.synthesis[n];
        }
        for (size_t n = 0; n < kHop; ++n) out[o][n] = toS16(ov[n]);
        std::memmove(ov.data(), ov.data() + kHop, (kWindow - kHop) * sizeof(float));
        std::fill(ov.end() - kHop, ov.end(), 0.0f);
    }
}

}  // namespace gaokun::micarray
