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
    static_assert(TAPS_PER_PHASE % 4 == 0, "4 taps at a time (Dot)");
    const Real pi = Real(3.14159265358979323846);
    const Real cutoff = Real(cutoff_d);
    const Real kaiser_beta = Real(kaiser_beta_d);
    const Real center = Real(0.5) * Real(num_taps - 1);
    const Real i0_beta = BesselI0(kaiser_beta);
    for (int i = 0; i < num_taps; ++i) {
      const Real x = Real(i) - center;
      const Real sinc = x == Real(0)
          ? Real(2) * cutoff
          : Sin(Real(2) * pi * cutoff * x) / (pi * x);
      const Real r = x / center;
      const Real window = BesselI0(
          kaiser_beta * std::sqrt(std::max<Real>(Real(0), Real(1) - r * r))) /
          i0_beta;
      // Gain of L compensates for the zero-stuffing.
      const Real h = sinc * window * Real(L);
      // Store per phase, the oldest sample's tap first, for a straight dot
      // product with the history (Dot).
      const int phase = i % L;
      const int q = i / L;
      phase_taps_[phase][TAPS_PER_PHASE - 1 - q] = static_cast<float>(h);
    }
    Reset();
  }

  void Reset() {
    std::fill(&history_[0], &history_[2 * kBufferSize], 0.0f);
    offset_ = 0;
  }

  // Lich port: processes a block of any size, carrying the position on the
  // intermediate (L x input rate) grid over to the next block. Returns the
  // number of output frames, which varies (e.g. 11, 11, 10 for 16 frames
  // in at 2/3). offset() is where the first output of the next block falls
  // on that grid, counted from its first input sample times L.
  size_t ProcessStreaming(
      const float* in_l, const float* in_r,
      float* out_l, float* out_r,
      size_t in_size) {
    Push(in_l, in_r, in_size);
    const size_t upsampled_size = in_size * L;
    size_t n = 0;
    size_t j = offset_;
    for (; j < upsampled_size; j += M) {
      Render(j, &out_l[n], &out_r[n]);
      ++n;
    }
    offset_ = j - upsampled_size;
    Shift(in_size);
    return n;
  }

  size_t offset() const { return offset_; }

  // Processes one block. in_size * L must be divisible by M. Returns the
  // number of output frames (in_size * L / M).
  size_t Process(
      const float* in_l, const float* in_r,
      float* out_l, float* out_r,
      size_t in_size) {
    Push(in_l, in_r, in_size);
    const size_t upsampled_size = in_size * L;
    size_t n = 0;
    for (size_t j = 0; j < upsampled_size; j += M) {
      Render(j, &out_l[n], &out_r[n]);
      ++n;
    }
    Shift(in_size);
    return n;
  }

  static int latency_in_input_samples() {
    return TAPS_PER_PHASE / 2;
  }

 private:
  // sin() for the filter design: keeps libm (and on the Lich, the C++
  // unwinder it pulls in) out of the build.
  static PEARLS_FILTER_DESIGN_REAL Sin(PEARLS_FILTER_DESIGN_REAL x) {
    typedef PEARLS_FILTER_DESIGN_REAL Real;
    const Real pi = Real(3.14159265358979323846);
    // Reduce to [-pi, pi], then to [-pi/2, pi/2].
    const int turns = int(x / (Real(2) * pi) + (x >= 0 ? Real(0.5) : Real(-0.5)));
    x -= Real(turns) * Real(2) * pi;
    if (x > pi / Real(2)) x = pi - x;
    if (x < -pi / Real(2)) x = -pi - x;
    // Taylor series to x^15: error < 1e-9 on [-pi/2, pi/2].
    const Real x2 = x * x;
    Real term = x;
    Real sum = x;
    for (int k = 1; k < 8; ++k) {
      term *= -x2 / (Real(2 * k) * Real(2 * k + 1));
      sum += term;
    }
    return sum;
  }

  // The history holds the last TAPS_PER_PHASE input frames, then the
  // block's, interleaved (left, right).
  inline void Push(const float* in_l, const float* in_r, size_t in_size) {
    float* x = &history_[2 * TAPS_PER_PHASE];
    for (size_t i = 0; i < in_size; ++i) {
      x[2 * i] = in_l[i];
      x[2 * i + 1] = in_r[i];
    }
  }

  // Keeps the last TAPS_PER_PHASE frames for the next block. The newest tap
  // can reach frame h + in_size - 1, the oldest h + in_size - h.
  inline void Shift(size_t in_size) {
    std::copy(
        &history_[2 * in_size], &history_[2 * (in_size + TAPS_PER_PHASE)],
        &history_[0]);
  }

  // Output at j on the intermediate grid: the phase's taps against the
  // TAPS_PER_PHASE frames up to the newest, h + j / L.
  inline void Render(size_t j, float* out_l, float* out_r) const {
    const int oldest = int(j / L) + 1;
    Dot(phase_taps_[j % L], &history_[2 * oldest], out_l, out_r);
  }

  // Lich port: 4 taps at a time, with two partial sums per channel. On the
  // Cortex-M4, in assembly: block loads, and multiplies and adds (1 cycle
  // each) rather than multiply-adds (3 cycles, which -ffast-math would
  // contract them into).
  static inline void Dot(
      const float* t, const float* x, float* out_l, float* out_r) {
    float l0 = 0.0f, r0 = 0.0f, l1 = 0.0f, r1 = 0.0f;
#if defined(__arm__) && defined(__ARM_FP)
    int n = TAPS_PER_PHASE;
    __asm__(
        "1:\n"
        "  vldmia %[t]!, {s0-s3}\n"
        "  vldmia %[x]!, {s4-s11}\n"
        "  vmul.f32 s12, s0, s4\n"
        "  vmul.f32 s13, s0, s5\n"
        "  vmul.f32 s14, s1, s6\n"
        "  vmul.f32 s15, s1, s7\n"
        "  vadd.f32 %[l0], %[l0], s12\n"
        "  vadd.f32 %[r0], %[r0], s13\n"
        "  vadd.f32 %[l1], %[l1], s14\n"
        "  vadd.f32 %[r1], %[r1], s15\n"
        "  vmul.f32 s12, s2, s8\n"
        "  vmul.f32 s13, s2, s9\n"
        "  vmul.f32 s14, s3, s10\n"
        "  vmul.f32 s15, s3, s11\n"
        "  vadd.f32 %[l0], %[l0], s12\n"
        "  vadd.f32 %[r0], %[r0], s13\n"
        "  vadd.f32 %[l1], %[l1], s14\n"
        "  vadd.f32 %[r1], %[r1], s15\n"
        "  subs %[n], %[n], #4\n"
        "  bne 1b\n"
        : [t] "+r"(t), [x] "+r"(x), [n] "+r"(n),
          [l0] "+t"(l0), [r0] "+t"(r0), [l1] "+t"(l1), [r1] "+t"(r1)
        :
        : "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10",
          "s11", "s12", "s13", "s14", "s15", "cc", "memory");
#else
    for (int q = 0; q < TAPS_PER_PHASE; q += 4) {
      l0 += t[q] * x[0] + t[q + 2] * x[4];
      r0 += t[q] * x[1] + t[q + 2] * x[5];
      l1 += t[q + 1] * x[2] + t[q + 3] * x[6];
      r1 += t[q + 1] * x[3] + t[q + 3] * x[7];
      x += 8;
    }
#endif
    *out_l = l0 + l1;
    *out_r = r0 + r1;
  }

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
  size_t offset_;
  float history_[2 * kBufferSize];
};

}  // namespace pearls

#endif  // PEARLS_ENGINE_RATIONAL_RESAMPLER_H_
