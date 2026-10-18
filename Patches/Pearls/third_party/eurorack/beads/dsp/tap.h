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
// Delay tap (delay + pitch-shifting).

#ifndef BEADS_DSP_TAP_H_
#define BEADS_DSP_TAP_H_

#include "stmlib/stmlib.h"

#include "stmlib/dsp/dsp.h"

#include "beads/dsp/delay_line.h"
#include "beads/dsp/frame.h"
#include "beads/dsp/delay_controller.h"

namespace beads {

// Lich port: what a tap reads over a block (see Tap::PrepareBlock).
struct TapBlockReads {
  int32_t position_1[kMaxHermiteBlock];
  int32_t position_2[kMaxHermiteBlock];
  float gain[kMaxHermiteBlock];
  float triangle[kMaxHermiteBlock];
  bool active[kMaxHermiteBlock];
  bool any;
};

template<int32_t window_size>
class Tap {
 public:
  Tap() { }
  ~Tap() { }
  
  void Init() {
    phase_ = 0;
  }
  
  template<typename DelayLineType>
  inline void Render(
      const DelayLineType* delay_line,
      int32_t delay,
      float gain,
      float speed,
      FloatFrame* out) {
    if (delay == 0) {
      return;
    }
    if (window_size == 1) {
      // Lich port: a silent tap reads nothing (same output).
      if (gain != 0.0f) {
        *out += gain * delay_line->template Read<INTERPOLATION_HERMITE>(delay);
      }
      return;
    }
    
    const int32_t full_window_size = 1024 * window_size;
    const int32_t half_window_size = full_window_size >> 1;
    
    phase_ += static_cast<int32_t>(int(1024.0f * (speed - 1.0f)));
    if (phase_ >= full_window_size) {
      phase_ -= full_window_size;
    } else if (phase_ < 0) {
      phase_ += full_window_size;
    }
    
    // Lich port: a silent tap only advances its phase (same output).
    if (gain == 0.0f) {
      return;
    }

    delay = std::max(full_window_size + 1, delay + half_window_size);
    
    const float phase = static_cast<float>(phase_) / float(full_window_size);
    const float triangle = phase < 0.5f ? 2.0f * phase : 2.0f - 2.0f * phase;
    
    const FloatFrame head_1 = delay_line->template Read<INTERPOLATION_HERMITE>(
          -phase_ + delay);
    const FloatFrame head_2 = delay_line->template Read<INTERPOLATION_HERMITE>(
          -((phase_ + half_window_size) % full_window_size) + delay);
    
    *out += (head_2 + (head_1 - head_2) * triangle) * gain;
  }

  // Lich port: Render() for a block, in three steps (see
  // MultitapDelay::RenderBlock): PrepareBlock() works out, for each sample s,
  // what Render() would read with the delay and gain of taps[s].tap[index]
  // (the gain times gain[s]) and speed[s]; the delay line reads it; and
  // AccumulateBlock() adds it to out[s]. Same output.
  __attribute__((noinline, noclone)) void PrepareBlock(
      const DelayController::Output* taps,
      int index,
      const float* gain,
      const float* speed,
      size_t size,
      TapBlockReads* r) {
    r->any = false;
    const int32_t full_window_size = 1024 * window_size;
    const int32_t half_window_size = full_window_size >> 1;
    for (size_t s = 0; s < size; ++s) {
      const DelayTap* tap = &taps[s].tap[index];
      r->active[s] = false;
      int32_t delay = tap->delay;
      if (delay == 0) {
        continue;
      }
      const float g = tap->gain * gain[s];
      if (window_size == 1) {
        if (g != 0.0f) {
          r->position_1[s] = delay;
          r->gain[s] = g;
          r->active[s] = r->any = true;
        }
        continue;
      }
      phase_ += static_cast<int32_t>(int(1024.0f * (speed[s] - 1.0f)));
      if (phase_ >= full_window_size) {
        phase_ -= full_window_size;
      } else if (phase_ < 0) {
        phase_ += full_window_size;
      }
      if (g == 0.0f) {
        continue;
      }
      delay = std::max(full_window_size + 1, delay + half_window_size);
      const float phase = static_cast<float>(phase_) / float(full_window_size);
      r->triangle[s] = phase < 0.5f ? 2.0f * phase : 2.0f - 2.0f * phase;
      r->position_1[s] = -phase_ + delay;
      r->position_2[s] = \
          -((phase_ + half_window_size) % full_window_size) + delay;
      r->gain[s] = g;
      r->active[s] = r->any = true;
    }
  }

  // head_1[s] and head_2[s]: what the delay line read at position_1[s] and
  // position_2[s] (only head_1 for the taps without pitch shifting).
  static __attribute__((noinline, noclone)) void AccumulateBlock(
      const TapBlockReads& r,
      const FloatFrame* head_1,
      const FloatFrame* head_2,
      FloatFrame* out,
      size_t size) {
    for (size_t s = 0; s < size; ++s) {
      if (!r.active[s]) {
        continue;
      }
      if (window_size == 1) {
        out[s] += r.gain[s] * head_1[s];
      } else {
        out[s] += (head_2[s] + (head_1[s] - head_2[s]) * r.triangle[s]) * \
            r.gain[s];
      }
    }
  }

  // RenderBlock() with gain[s] = 0 throughout: the phase only.
  inline void AdvanceBlock(
      const DelayController::Output* taps,
      int index,
      const float* speed,
      size_t size) {
    if (window_size == 1) {
      return;
    }
    const int32_t full_window_size = 1024 * window_size;
    for (size_t s = 0; s < size; ++s) {
      if (taps[s].tap[index].delay == 0) {
        continue;
      }
      phase_ += static_cast<int32_t>(int(1024.0f * (speed[s] - 1.0f)));
      if (phase_ >= full_window_size) {
        phase_ -= full_window_size;
      } else if (phase_ < 0) {
        phase_ += full_window_size;
      }
    }
  }

  // The range of positions Render() can read with delay (in 1/1024 frames).
  static inline void ReadRange(int32_t delay, int32_t* lo, int32_t* hi) {
    if (window_size == 1) {
      *lo = *hi = delay;
    } else {
      const int32_t full_window_size = 1024 * window_size;
      const int32_t d = std::max(full_window_size + 1,
                                 delay + (full_window_size >> 1));
      *lo = d - full_window_size;
      *hi = d;
    }
  }
  
 private:
  int32_t phase_;
  
  DISALLOW_COPY_AND_ASSIGN(Tap);
};

}  // namespace beads

#endif  // BEADS_DSP_TAP_H_
