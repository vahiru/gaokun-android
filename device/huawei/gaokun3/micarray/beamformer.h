/*
 * Fixed beamformer for the gaokun3 top microphones: three inputs, two outputs, 48 kHz.
 *
 * Short-time Fourier transform with a 1440-sample sine window and a 480-sample hop, a 2048-point
 * FFT, one complex 3x2 weight matrix per bin, and weighted overlap-add. The weights come from
 * Huawei's Windows capture effect (README.md); they are not part of this source tree.
 */
#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gaokun::micarray {

constexpr size_t kInputs = 3;
constexpr size_t kOutputs = 2;
constexpr size_t kHop = 480;
constexpr size_t kWindow = 1440;
constexpr size_t kFftSize = 2048;
constexpr size_t kBins = kFftSize / 2;  // DC to just below Nyquist; Nyquist is left at zero
// The output lags the input by the analysis history (kWindow - kHop) plus one hop of framing.
constexpr size_t kLatencyFrames = kWindow;

struct Weights {
    // [output][input][bin]
    std::complex<float> w[kOutputs][kInputs][kBins];
};

using HopIn = std::array<std::array<int16_t, kHop>, kInputs>;
using HopOut = std::array<std::array<int16_t, kHop>, kOutputs>;

// Reads the first matrix bank of the coefficient file and checks its size and XOR footer.
bool loadWeights(const char* path, Weights* weights, std::string* error);

class Beamformer {
  public:
    // 'weights' must outlive the beamformer.
    explicit Beamformer(const Weights& weights);

    void reset();
    // Any number of frames: 'in' holds 'channels' interleaved S16 channels, of which
    // map[0..2] are the beamformer's inputs; 'out' gets two interleaved S16 channels,
    // kLatencyFrames behind the input. No allocation.
    void process(const int16_t* in, size_t channels, const std::array<size_t, kInputs>& map,
                 int16_t* out, size_t frames);
    // The same in whole hops, for callers that do their own framing: one hop of the three inputs
    // in, that hop's two outputs out -- what process() hands out during the hop after it.
    // 'in' and 'out' may be the beamformer's own buffers. No allocation.
    void processHop(const HopIn& in, HopOut& out);

  private:
    const Weights& mWeights;
    size_t mFill = 0;  // frames of the current hop collected so far
    HopIn mIn{};
    HopOut mOut{};
    std::array<std::array<float, kWindow>, kInputs> mHistory{};
    std::array<std::array<float, kWindow>, kOutputs> mOverlap{};
    std::array<std::array<std::complex<float>, kBins>, kInputs> mSpectra{};
    std::array<std::complex<float>, kFftSize> mFft{};
};

}  // namespace gaokun::micarray
