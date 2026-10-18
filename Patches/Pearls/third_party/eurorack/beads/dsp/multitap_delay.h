// Copyright 2017 Emilie Gillet.
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
// Multitap delay.

#ifndef BEADS_DSP_MULTITAP_DELAY_H_
#define BEADS_DSP_MULTITAP_DELAY_H_

#include "stmlib/stmlib.h"
#include "stmlib/dsp/filter.h"
#include "stmlib/dsp/parameter_interpolator.h"

#include "beads/dsp/tap.h"
#include "beads/dsp/delay_controller.h"

#define ENABLE_BANDWIDTH_CONTROL

namespace beads {

class MultitapDelay {
 public:
  MultitapDelay() { }
  ~MultitapDelay() { }
  
  inline void Init(float sample_rate) {
    Reset();
    for (int i = 0; i < 2; ++i) {
      filter_[i].Init();
    }
  }
  
  inline void Reset() {
    for (int i = 0; i < DelayController::NUM_TAPS; ++i) {
      tap_[i].Init();
      no_latency_tap_[i].Init();
    }
  }
  
  template<typename DelayLineType>
  inline void Render(
      const DelayLineType* delay_line,
      DelayController::Output taps,
      float speed,
      float bandwidth,
      FloatFrame* out) {
    
    // Cross-fade between the original and pitch-shifted delays.
    const float gain = CrossfadeGain(speed);

    FloatFrame mix;
    mix.l = mix.r = 0.0f;
    for (int i = 0; i < DelayController::NUM_TAPS; ++i) {
      tap_[i].Render(
          delay_line,
          taps.tap[i].delay,
          taps.tap[i].gain * gain,
          speed,
          &mix);
      no_latency_tap_[i].Render(
          delay_line,
          taps.tap[i].delay,
          taps.tap[i].gain * (1.0f - gain),
          speed,
          &mix);
    }
    Finish(mix, bandwidth, out);
  }

  // Lich port: the tap mix of Render() for each sample s of a block, read
  // from delay_line (through read) at write position write_ptr[s], to be
  // passed to Finish() sample by sample. This is exact when no tap reads
  // frames the block writes (see CanRenderBlock): GranularProcessor then
  // renders the taps before writing the block, and each tap reads all its
  // frames at once. Otherwise, it renders one sample at a time, after each
  // write (size 1: as Render()).
  __attribute__((noinline, noclone)) void RenderBlock(
      HermiteBlockReader read,
      const void* delay_line,
      const DelayController::Output* taps,
      const float* speed,
      const int* write_ptr,
      FloatFrame* mix,
      size_t size) {
    float gain[kMaxHermiteBlock];
    float inverse_gain[kMaxHermiteBlock];
    for (size_t s = 0; s < size; ++s) {
      gain[s] = CrossfadeGain(speed[s]);
      inverse_gain[s] = 1.0f - gain[s];
      mix[s].l = mix[s].r = 0.0f;
    }
    // A tap whose gain is 0 over the block reads nothing (the pitch-shifted
    // ones still advance their phase).
    bool no_gain = true;
    bool no_inverse_gain = true;
    for (size_t s = 0; s < size; ++s) {
      no_gain = no_gain && gain[s] == 0.0f;
      no_inverse_gain = no_inverse_gain && inverse_gain[s] == 0.0f;
    }
    TapBlockReads reads;
    FloatFrame head_1[kMaxHermiteBlock];
    FloatFrame head_2[kMaxHermiteBlock];
    for (int i = 0; i < DelayController::NUM_TAPS; ++i) {
      if (no_gain) {
        tap_[i].AdvanceBlock(taps, i, speed, size);
      } else {
        tap_[i].PrepareBlock(taps, i, gain, speed, size, &reads);
        if (reads.any) {
          read(delay_line, reads.position_1, write_ptr, reads.active, head_1,
               size);
          read(delay_line, reads.position_2, write_ptr, reads.active, head_2,
               size);
          Tap<2048>::AccumulateBlock(reads, head_1, head_2, mix, size);
        }
      }
      if (!no_inverse_gain) {
        no_latency_tap_[i].PrepareBlock(
            taps, i, inverse_gain, speed, size, &reads);
        if (reads.any) {
          read(delay_line, reads.position_1, write_ptr, reads.active, head_1,
               size);
          Tap<1>::AccumulateBlock(reads, head_1, NULL, mix, size);
        }
      }
    }
  }

  // Whether the taps of a block only read frames older than the block (and
  // that the block does not overwrite), in a delay line of max_delay frames.
  static __attribute__((noinline, noclone)) bool CanRenderBlock(
      int max_delay,
      const DelayController::Output* taps,
      size_t size) {
    if (size > kMaxHermiteBlock) {
      return false;
    }
    const int32_t min_frames = int32_t(size) + 4;
    const int32_t max_frames = max_delay - int32_t(size) - 8;
    for (size_t s = 0; s < size; ++s) {
      for (int i = 0; i < DelayController::NUM_TAPS; ++i) {
        const int32_t delay = taps[s].tap[i].delay;
        if (delay == 0) {
          continue;
        }
        int32_t lo, hi;
        Tap<2048>::ReadRange(delay, &lo, &hi);
        if (delay < lo) lo = delay;
        if ((lo >> 10) < min_frames || (hi >> 10) > max_frames) {
          return false;
        }
      }
    }
    return true;
  }

  __attribute__((noinline)) void Finish(
      const FloatFrame& mix, float bandwidth, FloatFrame* out) {
#ifdef ENABLE_BANDWIDTH_CONTROL
    const float amount = fabsf(2.0f * bandwidth - 1.0f);
    
    // LP: -7 to -2 octaves
    // HP: -8 to -3 octaves
    const float octave = bandwidth < 0.5f
        ? -7.0f + bandwidth * 10.0f
        : -8.0f + (bandwidth - 0.5f) * 10.0f;
    const float f = 0.49f * stmlib::SemitonesToRatio(12.0f * octave);
    const float q = 1.0f + 16.0f * amount * f;
    const float wet_amount_sqrt = amount * (2.0f - amount);
    const float wet_amount = wet_amount_sqrt * (2.0f - wet_amount_sqrt);
    
    filter_[0].set_f_q<stmlib::FREQUENCY_FAST>(f, q);
    filter_[1].set(filter_[0]);
    
    FloatFrame wet;
    if (bandwidth < 0.5f) {
      wet.l = filter_[0].Process<stmlib::FILTER_MODE_LOW_PASS>(mix.l);
      wet.r = filter_[1].Process<stmlib::FILTER_MODE_LOW_PASS>(mix.r);
    } else {
      wet.l = filter_[0].Process<stmlib::FILTER_MODE_HIGH_PASS>(mix.l);
      wet.r = filter_[1].Process<stmlib::FILTER_MODE_HIGH_PASS>(mix.r);
    }
    *out += mix + (wet - mix) * wet_amount;
#else
    *out += mix;
#endif  // ENABLE_BANDWIDTH_CONTROL
  }

  // GranularProcessor's part: renders the tap mixes of a block before the
  // block is written, with the speed and bandwidth of each sample, when
  // CanRenderBlock(). write_ptr is the delay line's, before the block.
  // False, with nothing consumed, otherwise.
  __attribute__((noinline, noclone)) bool RenderTapsBlock(
      HermiteBlockReader read,
      const void* delay_line,
      int max_delay,
      int write_ptr,
      const DelayController::Output* taps,
      const bool* freeze,
      stmlib::ParameterInterpolator* speed_modulation,
      stmlib::ParameterInterpolator* bandwidth_modulation,
      FloatFrame* mix,
      float* bandwidth,
      size_t size) {
    if (!CanRenderBlock(max_delay, taps, size)) {
      return false;
    }
    float speed[kMaxHermiteBlock];
    int write_ptrs[kMaxHermiteBlock];
    for (size_t s = 0; s < size; ++s) {
      speed[s] = speed_modulation->Next();
      bandwidth[s] = bandwidth_modulation->Next();
      // The write position sample s reads at, after its Write() (as
      // DelayLine::AdvanceReadState).
      if (!freeze[s]) {
        write_ptr = write_ptr == 0 ? max_delay - 1 : write_ptr - 1;
      }
      write_ptrs[s] = write_ptr;
    }
    RenderBlock(read, delay_line, taps, speed, write_ptrs, mix, size);
    return true;
  }

 private:
  static inline float CrossfadeGain(float speed) {
    float gain = 100.0f * (fabsf(speed - 1.0f) - 0.01f);
    CONSTRAIN(gain, 0.0f, 1.0f);
    return gain;
  }

  Tap<2048> tap_[DelayController::NUM_TAPS];
  Tap<1> no_latency_tap_[DelayController::NUM_TAPS];
  stmlib::Svf filter_[2];

  DISALLOW_COPY_AND_ASSIGN(MultitapDelay);
};

}  // namespace beads

#endif  // BEADS_DSP_MULTITAP_DELAY_H_
