// Copyright 2021 Emilie Gillet.
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
// Automatic/manual input gain control with soft-clipping.
//
// Adapted from beads/gain_control.h for the Nebulae port: the only change is
// that the input is a FloatFrame (Csound/ALSA samples in [-1, 1]) instead of
// a 32-bit codec frame, so the codec driver header is not needed.

#ifndef PEARLS_ENGINE_GAIN_CONTROL_H_
#define PEARLS_ENGINE_GAIN_CONTROL_H_

#include "stmlib/stmlib.h"

#include "beads/dsp/frame.h"

namespace pearls {

class GainControl {
 public:
  GainControl() { }
  ~GainControl() { }

  enum SoftClippingMode {
    SOFT_CLIPPING_MODE_OFF,
    SOFT_CLIPPING_MODE_ON,
    SOFT_CLIPPING_MODE_DYNAMIC
  };

  void Init(float sample_rate, SoftClippingMode soft_clipping_mode, bool reset);

  void Process(
      bool active,
      uint8_t gain,
      bool left_normalization,
      bool right_normalization,
      const beads::FloatFrame* in,
      beads::FloatFrame* out,
      size_t size);

  inline uint8_t vu_meter() const {
    float intensity = peak_ * gain_;
    if (intensity > 1.0f) {
      return 255;
    } else {
      return uint8_t(192.0f * intensity * intensity);
    }
  }

  inline bool adjusting() const {
    return time_ < active_time_ && time_ > 0 && auto_gain_;
  }

  inline float gain() const { return gain_; }

 private:
  float peak_;
  float agc_peak_;
  float stable_agc_peak_;
  float gain_;

  float soft_clipping_amount_;
  SoftClippingMode soft_clipping_mode_;

  bool auto_gain_;
  float manual_gain_;

  float one_hertz_;

  size_t time_;
  size_t ignore_time_;
  size_t active_time_;

  DISALLOW_COPY_AND_ASSIGN(GainControl);
};

}  // namespace pearls

#endif  // PEARLS_ENGINE_GAIN_CONTROL_H_
