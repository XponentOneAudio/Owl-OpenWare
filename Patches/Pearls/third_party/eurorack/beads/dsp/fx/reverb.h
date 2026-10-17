// Copyright 2014 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
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
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Reverb.

#ifndef BEADS_DSP_FX_REVERB_H_
#define BEADS_DSP_FX_REVERB_H_

#include "stmlib/stmlib.h"

#include <algorithm>
#include <cmath>
#include "stmlib/dsp/parameter_interpolator.h"

#include "beads/dsp/fx/fx_engine.h"

// Lich port: the reverb can run at a fraction of the sample rate (divider
// 2: 24 kHz, with half the delay memory, which then fits in the Lich's
// internal RAM). ReverbT<1> is Beads' reverb.
#ifndef BEADS_REVERB_DIVIDER
#define BEADS_REVERB_DIVIDER 1
#endif

namespace beads {

template<int divider>
class ReverbT {
 public:
  ReverbT() { }
  ~ReverbT() { }

  // Delay memory, in samples (16 bits each).
  enum { kBufferSize = 32768 / divider };
  
  void Init(uint16_t* buffer, float sample_rate) {
    engine_.Init(buffer);
    Reset(sample_rate);
  }
  
  void Reset(float sample_rate) {
    engine_.SetLFOFrequency(LFO_1, 0.5f * float(divider) / sample_rate);
    engine_.SetLFOFrequency(LFO_2, 0.3f * float(divider) / sample_rate);
    lp_ = 0.7f;
    lp_1_ = 0.0f;
    lp_2_ = 0.0f;
    mix_ = 0.0f;
    dry_ = 0.0f;
    wet_ = 0.0f;
    send_ = 0.0f;
    silent_samples_ = 0;
    previous_wet_l_ = previous_wet_r_ = 0.0f;
  }
  
  void Process(FloatFrame* in_out, size_t size) {
    // This is the Griesinger topology described in the Dattorro paper
    // (4 AP diffusers on the input, then a loop of 2x 2AP+1Delay).
    // Modulation is applied in the loop of the first diffuser AP for additional
    // smearing; and to the two long delays for a slow shimmer/chorus effect.
#define D(n) (((n) + divider / 2) / divider)
    typedef typename E::template Reserve<D(150),
      typename E::template Reserve<D(214),
      typename E::template Reserve<D(319),
      typename E::template Reserve<D(527),
      typename E::template Reserve<D(2182),
      typename E::template Reserve<D(2690),
      typename E::template Reserve<D(4501),
      typename E::template Reserve<D(2525),
      typename E::template Reserve<D(2197),
      typename E::template Reserve<D(6312)> > > > > > > > > > Memory;
#undef D
    typename E::template DelayLine<Memory, 0> ap1;
    typename E::template DelayLine<Memory, 1> ap2;
    typename E::template DelayLine<Memory, 2> ap3;
    typename E::template DelayLine<Memory, 3> ap4;
    typename E::template DelayLine<Memory, 4> dap1a;
    typename E::template DelayLine<Memory, 5> dap1b;
    typename E::template DelayLine<Memory, 6> del1;
    typename E::template DelayLine<Memory, 7> dap2a;
    typename E::template DelayLine<Memory, 8> dap2b;
    typename E::template DelayLine<Memory, 9> del2;
    typename E::Context c;
    const float fd = float(divider);

    const float kap = 0.625f;
    // The damping filter runs once per reverb sample.
    const float klp = divider == 1 ? lp_ : 1.0f - (1.0f - lp_) * (1.0f - lp_);
    const float krt = reverb_time_;

    const float input_gain = 0.2f * (1.0f - 0.333f * reverb_time_);
    const float output_gain = 0.4f / input_gain;
    
    // const float send_amount = 1.0f;
    // const float wet = mix_ * (2.0f - mix_) * output_gain;
    
    const float send_amount_sqrt = mix_ * (2.0f - mix_);
    const float send_amount = send_amount_sqrt * (2.0f - send_amount_sqrt);
    const float wet = output_gain;
    const float dry = (1.0f - mix_) * (1.0f + mix_);
    
#ifndef BEADS_REVERB_ALWAYS_ON
    // Lich port: with nothing sent to the reverb (mix 0) and a tail that has
    // stayed under -90 dB for a second, the reverb only adds silence: skip
    // it. The output differs from Beads by less than 3e-5.
    if (send_amount == 0.0f && send_ == 0.0f && dry == 1.0f &&
        fabsf(dry_ - 1.0f) < 1e-5f &&
        silent_samples_ >= kSilentSamples) {
      return;
    }
#endif  // BEADS_REVERB_ALWAYS_ON
    float tail_peak = 0.0f;
    const int block_size = int(size);

    stmlib::ParameterInterpolator wet_modulation(&wet_, wet, size);
    stmlib::ParameterInterpolator dry_modulation(&dry_, dry, size);
    const size_t reverb_size = size / divider;
    stmlib::ParameterInterpolator send_modulation(
        &send_,
        send_amount * input_gain,
        reverb_size);

    for (size_t k = 0; k < reverb_size; ++k) {
      const float send_amount = send_modulation.Next();
      
      float wet;
      float wet_l;
      float apout = 0.0f;
      engine_.Start(&c);
      
      // Smear AP1 inside the loop.
      c.Interpolate(ap1, 10.0f / fd, LFO_1, 80.0f / fd, 1.0f);
      c.Write(ap1, 100 / divider, 0.0f);
      
      float input = in_out->l + in_out->r;
      if (divider == 2) {
        input = 0.5f * (input + in_out[1].l + in_out[1].r);
      }
      c.Read(input, send_amount);
      
      // Diffuse through 4 allpasses.
      c.Read(ap1 TAIL, kap);
      c.WriteAllPass(ap1, -kap);
      c.Read(ap2 TAIL, kap);
      c.WriteAllPass(ap2, -kap);
      c.Read(ap3 TAIL, kap);
      c.WriteAllPass(ap3, -kap);
      c.Read(ap4 TAIL, kap);
      c.WriteAllPass(ap4, -kap);
      c.Write(apout);
      
      // Main reverb loop.
      c.Load(apout);
      c.Interpolate(del2, 6261.0f / fd, LFO_2, 50.0f / fd, krt);
      c.Lp(lp_1_, klp);
      c.Read(dap1a TAIL, -kap);
      c.WriteAllPass(dap1a, kap);
      c.Read(dap1b TAIL, kap);
      c.WriteAllPass(dap1b, -kap);
      c.Write(del1, 1.0f);
      c.Write(wet_l);

      c.Load(apout);
      c.Interpolate(del1, 4460.0f / fd, LFO_1, 40.0f / fd, krt);
      c.Lp(lp_2_, klp);
      c.Read(dap2a TAIL, kap);
      c.WriteAllPass(dap2a, -kap);
      c.Read(dap2b TAIL, -kap);
      c.WriteAllPass(dap2b, kap);
      c.Write(del2, 1.0f);
      c.Write(wet);

      if (divider == 1) {
        const float dry_amount = dry_modulation.Next();
        const float wet_amount = wet_modulation.Next();
        in_out->l = in_out->l * dry_amount + wet_l * wet_amount;
        in_out->r = in_out->r * dry_amount + wet * wet_amount;
        tail_peak = std::max(tail_peak, fabsf(wet * wet_amount));
        ++in_out;
      } else {
        // Back to the full rate: linear interpolation.
        for (int j = 0; j < divider; ++j) {
          const float t = float(j + 1) / fd;
          const float l = previous_wet_l_ + (wet_l - previous_wet_l_) * t;
          const float r = previous_wet_r_ + (wet - previous_wet_r_) * t;
          const float dry_amount = dry_modulation.Next();
          const float wet_amount = wet_modulation.Next();
          in_out->l = in_out->l * dry_amount + l * wet_amount;
          in_out->r = in_out->r * dry_amount + r * wet_amount;
          tail_peak = std::max(tail_peak, fabsf(r * wet_amount));
          ++in_out;
        }
        previous_wet_l_ = wet_l;
        previous_wet_r_ = wet;
      }
    }
    silent_samples_ = tail_peak < 3e-5f
        ? std::min(silent_samples_ + block_size, int(kSilentSamples))
        : 0;
  }
  
  inline void set_mix(float mix) {
    mix_ = mix;
  }
  
  inline void set_time(float reverb_time) {
    reverb_time_ = reverb_time;
  }
  
  inline void set_lp(float lp) {
    lp_ = lp;
  }
  
  inline void Clear() {
    engine_.Clear();
  }
  
 private:
  typedef FxEngine<kBufferSize, FORMAT_16_BIT> E;
  E engine_;
  
  float mix_;
  float wet_;
  float dry_;
  float send_;
  float reverb_time_;
  float lp_;
  
  float lp_1_;
  float lp_2_;

  // Lich port: tail silence detection.
  enum { kSilentSamples = 48000 };
  int silent_samples_;
  float previous_wet_l_;
  float previous_wet_r_;
  
  DISALLOW_COPY_AND_ASSIGN(ReverbT);
};

typedef ReverbT<1> Reverb;

}  // namespace beads

#endif  // BEADS_DSP_FX_REVERB_H_
