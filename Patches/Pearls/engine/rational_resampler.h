// Copyright 2026 The Pearls (Beads for Nebulae) contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// -----------------------------------------------------------------------------
//
// Polyphase rational resampler (up by L, down by M) used to run the 32 kHz
// "vintage digital" Beads processor from the fixed 48 kHz Nebulae audio
// stream: 48 kHz -> 32 kHz is L = 2, M = 3, and back is L = 3, M = 2.
//
// The anti-aliasing / anti-imaging filter is a Kaiser-windowed sinc designed
// at the intermediate rate (96 kHz), with its cutoff a little below the
// 16 kHz Nyquist frequency of the 32 kHz stream. That roughly matches the
// band-limiting the Beads codec applies at 32 kHz.

#ifndef PEARLS_ENGINE_RATIONAL_RESAMPLER_H_
#define PEARLS_ENGINE_RATIONAL_RESAMPLER_H_

#include <algorithm>
#include <cmath>
#include <cstddef>

#ifndef PEARLS_FILTER_DESIGN_REAL
#define PEARLS_FILTER_DESIGN_REAL double
#endif

namespace pearls {

// Number of filter taps at the intermediate rate. Must be divisible by L.
template<int L, int M, int num_taps, int max_block_size>
class RationalResampler {
 public:
  enum {
    TAPS_PER_PHASE = num_taps / L,
  };

  RationalResampler() { }
  ~RationalResampler() { }

  // cutoff: normalised to the intermediate rate (cycles/sample, < 0.5).
  // The filter is designed in PEARLS_FILTER_DESIGN_REAL (double; the Lich
  // uses float, which keeps double-precision libm out of its patch).
  void Init(double cutoff_d, double kaiser_beta_d) {
    typedef PEARLS_FILTER_DESIGN_REAL Real;
    static_assert(num_taps % L == 0, "num_taps must be divisible by L");
    const Real pi = Real(3.14159265358979323846);
    const Real cutoff = Real(cutoff_d);
    const Real kaiser_beta = Real(kaiser_beta_d);
    const Real center = Real(0.5) * Real(num_taps - 1);
    const Real i0_beta = BesselI0(kaiser_beta);
    for (int i = 0; i < num_taps; ++i) {
      const Real x = Real(i) - center;
      const Real sinc = x == Real(0)
          ? Real(2) * cutoff
          : std::sin(Real(2) * pi * cutoff * x) / (pi * x);
      const Real r = x / center;
      const Real window = BesselI0(
          kaiser_beta * std::sqrt(std::max<Real>(Real(0), Real(1) - r * r))) /
          i0_beta;
      // Gain of L compensates for the zero-stuffing.
      const Real h = sinc * window * Real(L);
      // Store per phase, reversed, for a straight dot product.
      const int phase = i % L;
      const int q = i / L;
      phase_taps_[phase][q] = static_cast<float>(h);
    }
    Reset();
  }

  void Reset() {
    std::fill(&history_[0][0], &history_[0][0] + 2 * kBufferSize, 0.0f);
  }

  // Processes one block. in_size * L must be divisible by M. Returns the
  // number of output frames (in_size * L / M).
  size_t Process(
      const float* in_l, const float* in_r,
      float* out_l, float* out_r,
      size_t in_size) {
    const int h = TAPS_PER_PHASE;
    std::copy(in_l, in_l + in_size, &history_[0][h]);
    std::copy(in_r, in_r + in_size, &history_[1][h]);

    const size_t upsampled_size = in_size * L;
    size_t n = 0;
    for (size_t j = 0; j < upsampled_size; j += M) {
      const int phase = j % L;
      // Index (in the history buffer) of the newest input sample that
      // contributes to this output.
      const int newest = h + int(j / L);
      const float* taps = phase_taps_[phase];
      float acc_l = 0.0f;
      float acc_r = 0.0f;
      for (int q = 0; q < h; ++q) {
        acc_l += taps[q] * history_[0][newest - q];
        acc_r += taps[q] * history_[1][newest - q];
      }
      out_l[n] = acc_l;
      out_r[n] = acc_r;
      ++n;
    }

    // Keep the last TAPS_PER_PHASE samples for the next block. The newest
    // tap can reach index h + in_size - 1, the oldest h + in_size - h.
    for (int c = 0; c < 2; ++c) {
      std::copy(
          &history_[c][in_size], &history_[c][in_size + h], &history_[c][0]);
    }
    return n;
  }

  static int latency_in_input_samples() {
    return TAPS_PER_PHASE / 2;
  }

 private:
  static PEARLS_FILTER_DESIGN_REAL BesselI0(PEARLS_FILTER_DESIGN_REAL x) {
    typedef PEARLS_FILTER_DESIGN_REAL Real;
    Real sum = 1;
    Real term = 1;
    const Real y = x * x / Real(4);
    for (int k = 1; k < 64; ++k) {
      term *= y / (Real(k) * Real(k));
      sum += term;
      if (term < Real(1e-12) * sum) {
        break;
      }
    }
    return sum;
  }

  static const int kBufferSize = TAPS_PER_PHASE + max_block_size + 1;

  float phase_taps_[L][TAPS_PER_PHASE];
  float history_[2][kBufferSize];
};

}  // namespace pearls

#endif  // PEARLS_ENGINE_RATIONAL_RESAMPLER_H_
