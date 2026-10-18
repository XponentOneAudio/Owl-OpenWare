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
// Granular processor.

#ifndef BEADS_DSP_GRANULAR_PROCESSOR_H_
#define BEADS_DSP_GRANULAR_PROCESSOR_H_

#include "stmlib/stmlib.h"
#include "stmlib/dsp/filter.h"
#include "stmlib/dsp/parameter_interpolator.h"
#include "stmlib/dsp/sample_rate_converter.h"

#include "beads/dsp/delay_granular_player.h"
#include "beads/dsp/limiter.h"
#include "beads/dsp/multitap_delay.h"
#include "beads/dsp/sample_rate_conversion_filters.h"

namespace beads {

template<Resolution resolution, int max_delay, int ratio>
class GranularProcessor {
 public:
  GranularProcessor() { }
  ~GranularProcessor() { }
  
  inline void Init(
      float sample_rate,
      uint8_t* buffers[]) {
    mono_delay_line_.Init(buffers);
    stereo_delay_line_.Init(buffers);

#ifndef BEADS_NO_WASTE_CPU
    dummy_granular_player_.Init(sample_rate);
#endif  // BEADS_NO_WASTE_CPU
    granular_player_.Init(sample_rate);
    multitap_delay_.Init(sample_rate);
    
    feedback_ = 0.0f;
    shape_ = 0.0f;
    speed_ = 0.0f;
    bandwidth_ = 0.0f;
    previous_sample_ = 0.0f;
    
    for (size_t i = 0; i < 2; ++i) {
      dc_blocker_[i].Init();
      dc_blocker_[i].template set_f_q<stmlib::FREQUENCY_FAST>(
          40.0f / sample_rate, 0.5f);

      src_down_[i].Init();
      src_up_[i].Init();
    }
    
    pitch_modulation_.Init(0.2f / sample_rate);
    input_limiter_.Init(1.0f / sample_rate, 0.95f);
  }
  
  inline void Reset() {
    mono_delay_line_.Reset();
    stereo_delay_line_.Reset();
    granular_player_.Reset();
    multitap_delay_.Reset();
  }
  
  inline int max_delay_times_ratio() const {
    return max_delay * ratio;
  }
  
  inline void Trim(bool mono) {
    if (mono) {
      mono_delay_line_.Trim();
    } else {
      stereo_delay_line_.Trim();
    }
  }
  
  inline int* mutable_delay_write_ptr(bool mono) {
    return mono
        ? mono_delay_line_.mutable_write_ptr()
        : stereo_delay_line_.mutable_write_ptr();
  }
  
  inline int remaining(bool mono) const {
    return mono
        ? mono_delay_line_.remaining()
        : stereo_delay_line_.remaining();
  }
  
  // Nebulae port: direct access to the recording buffer, at its own sample
  // rate, for loading and saving its contents. Delay 1 is the most recent
  // frame, duration() the oldest.
  inline int buffer_duration(bool mono) const {
    return mono ? mono_delay_line_.duration() : stereo_delay_line_.duration();
  }

  inline int buffer_capacity(bool mono) const {
    return mono ? 2 * max_delay : max_delay;
  }

  inline FloatFrame ReadBuffer(bool mono, int delay) const {
    return mono
        ? mono_delay_line_.template Read<INTERPOLATION_ZOH>(delay, 0.0f)
        : stereo_delay_line_.template Read<INTERPOLATION_ZOH>(delay, 0.0f);
  }

  inline void WriteBuffer(bool mono, FloatFrame frame) {
    if (mono) {
      mono_delay_line_.Write(frame);
    } else {
      stereo_delay_line_.Write(frame);
    }
  }

  // Lich port: grain voice limit (see GranularPlayer::set_max_grains).
  inline void set_max_grains(int max_grains) {
    granular_player_.set_max_grains(max_grains);
  }

  inline uint8_t* delay_buffer_raw_bytes(bool mono, size_t offset) {
    // Independently of the stereo/mono setting, we're writing in the same
    // bytes anyway!
    return mono_delay_line_.raw_bytes(offset);
  }
  
#ifndef BEADS_NO_WASTE_CPU
  inline void WasteCPUCycles(size_t size) {
    Parameters p;
  
    p.time.base = 0.0f;
    p.time.attenuverter = 1.0f;
    p.time.has_cv = false;

    p.size.base = 0.0f;
    p.size.attenuverter = 1.0f;
    p.size.has_cv = false;

    p.shape.base = 0.0f;
    p.shape.attenuverter = 1.0f;
    p.shape.has_cv = false;

    p.reverse_probability = 0.0f;
    p.stereo_spread = 0.0f;

    p.pitch.base = 0.0f;
    p.pitch.attenuverter = 1.0f;
    p.pitch.has_cv = false;
    p.pitch.scale_mask = 0;
    
    p.monophonic_mode = false;

    FloatFrame out;
    out.l = 0.0f;
    out.r = 0.0f;
    
    for (size_t i = 0; i < size; ++i) {
      TriggerGenerator::Output trigger;
      trigger.trigger = i == 0;
      trigger.fractional_sample = 0.0f;
      
      dummy_granular_player_.Render(&stereo_delay_line_, trigger, p, &out);
    }
  }
#endif  // BEADS_NO_WASTE_CPU
  
  inline void Process(
      const TriggerGenerator::Output* triggers,
      const DelayController::Output* taps,
      const bool* freeze,
      const Parameters& parameters,
      const float feedback,
      const bool mono,
      const FloatFrame* in,
      FloatFrame* out,
      size_t size) {
    if (ratio == 1) {
      ProcessInternal(
          triggers, taps, freeze, parameters, feedback, mono, in, out, size);
    } else {
      size_t d_size = size / size_t(ratio);

      bool d_freeze[d_size];
      TriggerGenerator::Output d_triggers[d_size];
      DelayController::Output d_taps[d_size];
      FloatFrame d_in[d_size];
      FloatFrame d_out[d_size];
      float downsampled[d_size];
      float upsampled[size];

      DownsampleGate(freeze, d_freeze, size);
      
      if (triggers) {
        DownsampleTriggers(triggers, d_triggers, size);
      }
      if (taps) {
        DownsampleTaps(taps, d_taps, size);
      }
      
      for (size_t channel = 0; channel < 2; ++channel) {
        for (size_t i = 0; i < size; ++i) {
          upsampled[i] = *(&(in[i].l) + channel);
        }
        src_down_[channel].Process(upsampled, downsampled, size);
        for (size_t i = 0; i < d_size; ++i) {
          *(&(d_in[i].l) + channel) = downsampled[i];
        }
      }

      ProcessInternal(
          triggers ? d_triggers : NULL,
          taps ? d_taps : NULL,
          d_freeze,
          parameters, feedback,
          mono, d_in, d_out, d_size);
      
      for (size_t channel = 0; channel < 2; ++channel) {
        for (size_t i = 0; i < d_size; ++i) {
          downsampled[i] = *(&(d_out[i].l) + channel);
        }
        src_up_[channel].Process(downsampled, upsampled, d_size);
        for (size_t i = 0; i < size; ++i) {
          *(&(out[i].l) + channel) = upsampled[i];
        }
      }
    }
  }
  
 private:
  inline void DownsampleGate(const bool* in, bool* out, size_t size) {
    for (size_t i = 0; i < size / ratio; ++i) {
      bool output = false;
      for (size_t j = 0; j < ratio; ++j) {
        output |= *in;
        ++in;
      }
      *out = output;
      ++out;
    }
  }
  
  inline void DownsampleTaps(
      const DelayController::Output* in,
      DelayController::Output* out,
      size_t size) {
    for (size_t i = 0; i < size; i += ratio) {
      for (size_t j = 0; j < DelayController::NUM_TAPS; ++j) {
        out->tap[j].delay = in[i].tap[j].delay / ratio;
        out->tap[j].gain = in[i].tap[j].gain;
      }
      ++out;
    }
  }
  
  inline void DownsampleTriggers(
      const TriggerGenerator::Output* in,
      TriggerGenerator::Output* out,
      size_t size) {
    float inv_ratio = 1.0f / static_cast<float>(ratio);
    for (size_t i = 0; i < size / ratio; ++i) {
      out->regularity = in->regularity;

      bool has_trigger = false;
      float fractional_sample = 0.0f;
      bool gate = false;
      for (size_t j = 0; j < ratio; ++j) {
        if (in->trigger && !has_trigger) {
          has_trigger = true;
          fractional_sample = (in->fractional_sample + float(j)) * inv_ratio;
        }
        gate |= in->gate;
        ++in;
      }
      out->trigger = has_trigger;
      out->gate = gate;
      out->fractional_sample = fractional_sample;
      ++out;
    }
  }

  inline void ProcessInternal(
      const TriggerGenerator::Output* triggers,
      const DelayController::Output* taps,
      const bool* freeze,
      const Parameters& parameters,
      const float feedback,
      const bool mono,
      const FloatFrame* in,
      FloatFrame* out,
      size_t size) {
    const float speed = stmlib::SemitonesToRatio(
        parameters.pitch.value(&pitch_modulation_, size, 24.0f, false));

    float bandwidth = 0.5f * (1.0f + parameters.size.attenuverter) + \
        (parameters.size.has_cv ? parameters.size.cv : 0.0f);
    CONSTRAIN(bandwidth, 0.0f, 1.0f)

    stmlib::ParameterInterpolator bandwidth_modulation(
        &bandwidth_, bandwidth, size);
    stmlib::ParameterInterpolator feedback_modulation(
        &feedback_, feedback, size);
    stmlib::ParameterInterpolator speed_modulation(&speed_, speed, size);
    
    const float max_feedback_gain = taps ? 1.5f : 3.0f;

    const float lp_coefficients[RESOLUTION_LAST] = { 0.75f, 0.9f, 0.999f };
    const float soft_limit_gains[RESOLUTION_LAST] = { 1.75f, 1.3f, 0.0f };

    const float lp_coefficient = lp_coefficients[resolution];
    const float soft_limit_gain = soft_limit_gains[resolution];

#ifndef BEADS_PER_SAMPLE_GRAINS
    // Lich port: the grains are rendered for the whole block first, then the
    // block is written. This is exact because grains never read within
    // DelayGrain::head_crossing_safety_zone (256) frames of the write head.
    // Only a grain fading out at the far end of the buffer (the last frames
    // before they are overwritten, where its gain is under 1%) sees the old
    // contents instead of the new ones.
    const bool grains_rendered = triggers != NULL;
    if (grains_rendered) {
      std::fill(&out[0], &out[size], FloatFrame());
      if (mono) {
        RenderGrainsBlock(
            &mono_delay_line_, triggers, freeze, parameters, out, size);
      } else {
        RenderGrainsBlock(
            &stereo_delay_line_, triggers, freeze, parameters, out, size);
      }
    }
    // Lich port: the taps too, when they read no frame the block writes.
    FloatFrame tap_mix[size ? size : 1];
    float tap_bandwidth[size ? size : 1];
    bool taps_rendered = false;
    if (taps && size) {
      taps_rendered = mono
          ? RenderTapsBlock(&mono_delay_line_, taps, freeze, &speed_modulation,
                            &bandwidth_modulation, tap_mix, tap_bandwidth,
                            size)
          : RenderTapsBlock(&stereo_delay_line_, taps, freeze,
                            &speed_modulation, &bandwidth_modulation, tap_mix,
                            tap_bandwidth, size);
    }
    size_t sample = 0;
#else
    const bool grains_rendered = false;
    const bool taps_rendered = false;
    FloatFrame tap_mix[1];
    float tap_bandwidth[1];
    size_t sample = 0;
#endif  // BEADS_PER_SAMPLE_GRAINS

    while (size--) {
      const float feedback = feedback_modulation.Next();
      FloatFrame input = *in;
      FloatFrame s = input;
      
      if (soft_limit_gain) {
        const float gain = max_feedback_gain / soft_limit_gain;
        s += previous_sample_ * feedback * gain;
      } else {
        s += previous_sample_ * feedback * feedback * max_feedback_gain;
      }
      input_limiter_.Process(&s, 1);

      if (soft_limit_gain) {
        s *= soft_limit_gain;
        s.l = stmlib::SoftClip(s.l);
        s.r = stmlib::SoftClip(s.r);
        s = input + feedback * (s - input);
      }

      if (!grains_rendered) {
        out->l = 0.0f;
        out->r = 0.0f;
      }
      if (mono) {
        mono_delay_line_.Write(s, *freeze);
        if (triggers && !grains_rendered) {
          granular_player_.Render(
              &mono_delay_line_,
              *triggers,
              parameters,
              out);
        }
        if (taps && taps_rendered) {
          multitap_delay_.Finish(tap_mix[sample], tap_bandwidth[sample], out);
        } else if (taps) {
#ifndef BEADS_PER_SAMPLE_GRAINS
          const float speed = speed_modulation.Next();
          RenderTapsSample(
              &mono_delay_line_, taps, speed, bandwidth_modulation.Next(),
              out);
#else
          multitap_delay_.Render(
              &mono_delay_line_,
              *taps,
              speed_modulation.Next(),
              bandwidth_modulation.Next(),
              out);
#endif  // BEADS_PER_SAMPLE_GRAINS
        }
      } else {
        stereo_delay_line_.Write(s, *freeze);
        if (triggers && !grains_rendered) {
          granular_player_.Render(
              &stereo_delay_line_,
              *triggers,
              parameters,
              out);
        }
        if (taps && taps_rendered) {
          multitap_delay_.Finish(tap_mix[sample], tap_bandwidth[sample], out);
        } else if (taps) {
#ifndef BEADS_PER_SAMPLE_GRAINS
          const float speed = speed_modulation.Next();
          RenderTapsSample(
              &stereo_delay_line_, taps, speed, bandwidth_modulation.Next(),
              out);
#else
          multitap_delay_.Render(
              &stereo_delay_line_,
              *taps,
              speed_modulation.Next(),
              bandwidth_modulation.Next(),
              out);
#endif  // BEADS_PER_SAMPLE_GRAINS
        }
      }
      
      ONE_POLE(
          previous_sample_.l,
          dc_blocker_[0].template Process<stmlib::FILTER_MODE_HIGH_PASS>(
              out->l),
          lp_coefficient);
      ONE_POLE(
          previous_sample_.r,
          dc_blocker_[1].template Process<stmlib::FILTER_MODE_HIGH_PASS>(
              out->r),
          lp_coefficient);

      if (triggers) {
        ++triggers;
      }
      if (taps) {
        ++taps;
      }

      ++freeze;
      ++in;
      ++out;
      ++sample;
    }
    (void)sample;
    (void)tap_mix;
    (void)tap_bandwidth;
  }

  // Lich port: the tap mixes of a block, before the block is written (see
  // MultitapDelay::RenderTapsBlock).
  template<typename DelayLineType>
  inline bool RenderTapsBlock(
      const DelayLineType* delay_line,
      const DelayController::Output* taps,
      const bool* freeze,
      stmlib::ParameterInterpolator* speed_modulation,
      stmlib::ParameterInterpolator* bandwidth_modulation,
      FloatFrame* mix,
      float* bandwidth,
      size_t size) {
    return multitap_delay_.RenderTapsBlock(
        &DelayLineType::ReadHermiteBlockOf, delay_line,
        DelayLineType::MAX_DELAY, delay_line->write_ptr(), taps, freeze,
        speed_modulation, bandwidth_modulation, mix, bandwidth, size);
  }

  // Lich port: the taps of one sample, after its Write(): as Render().
  template<typename DelayLineType>
  __attribute__((noinline)) void RenderTapsSample(
      const DelayLineType* delay_line,
      const DelayController::Output* taps,
      float speed,
      float bandwidth,
      FloatFrame* out) {
    FloatFrame mix;
    const int write_ptr = delay_line->write_ptr();
    multitap_delay_.RenderBlock(
        &DelayLineType::ReadHermiteBlockOf, delay_line, taps, &speed,
        &write_ptr, &mix, 1);
    multitap_delay_.Finish(mix, bandwidth, out);
  }
      
  // Lich port: renders the grains of a block before the block is written.
  // The block is cut at triggers (a grain starts with the delay line state of
  // its sample) and at freeze changes; each segment is rendered grain by
  // grain.
  template<typename DelayLineType>
  inline void RenderGrainsBlock(
      DelayLineType* delay_line,
      const TriggerGenerator::Output* triggers,
      const bool* freeze,
      const Parameters& parameters,
      FloatFrame* out,
      size_t size) {
    const typename DelayLineType::ReadState saved = delay_line->read_state();
    typename DelayLineType::ReadState state = saved;
    size_t start = 0;
    while (start < size) {
      // The state seen by sample `start`, after its Write().
      delay_line->AdvanceReadState(&state, freeze[start]);
      if (triggers[start].trigger) {
        delay_line->set_read_state(state);
        granular_player_.StartGrain(delay_line, triggers[start], parameters);
      }
      size_t end = start + 1;
      while (end < size && !triggers[end].trigger &&
             freeze[end] == freeze[start]) {
        ++end;
      }
      granular_player_.RenderSegment(
          delay_line, state.write_ptr, state.frozen, &out[start], end - start);
      for (size_t i = start + 1; i < end; ++i) {
        delay_line->AdvanceReadState(&state, freeze[i]);
      }
      start = end;
    }
    delay_line->set_read_state(saved);
  }

#ifndef BEADS_NO_WASTE_CPU
  // Lich port: only used to waste CPU cycles (BEADS_NO_WASTE_CPU removes it,
  // saving 2.4 kB of RAM per quality setting).
  DelayGranularPlayer dummy_granular_player_;
#endif  // BEADS_NO_WASTE_CPU
  DelayGranularPlayer granular_player_;
  MultitapDelay multitap_delay_;
  
  DelayLine<resolution, max_delay, 2> stereo_delay_line_;
  DelayLine<resolution, max_delay * 2, 1> mono_delay_line_;
  
  stmlib::SampleRateConverter<stmlib::SRC_DOWN, ratio, 48> src_down_[2];
  stmlib::SampleRateConverter<stmlib::SRC_UP, ratio, 48> src_up_[2];
  
  float feedback_;
  float shape_;
  float speed_;
  float bandwidth_;
  FloatFrame previous_sample_;
  
  stmlib::Svf dc_blocker_[2];
  
  RandomWalkModulation pitch_modulation_;
  
  Limiter input_limiter_;
    
  DISALLOW_COPY_AND_ASSIGN(GranularProcessor);
};

}  // namespace beads

#endif  // BEADS_DSP_GRANULAR_PROCESSOR_H_
