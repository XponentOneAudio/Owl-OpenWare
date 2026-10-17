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
// Granular player.

#ifndef BEADS_DSP_GRANULAR_PLAYER_H_
#define BEADS_DSP_GRANULAR_PLAYER_H_

#include "stmlib/stmlib.h"

#include "beads/dsp/grain.h"
#include "beads/dsp/randomized_parameter.h"
#include "beads/dsp/trigger_generator.h"

namespace beads {

// These three constants can be adjusted, but the inequality:
// kInstantCrossfadeTime < kSimpleSchedulerThreshold < kSimpleSchedulerThreshold
// must be preserved (the code relies on that inequality to avoid std::sort).
const float kInstantCrossfadeTime = 0.005f;
const float kSimpleSchedulerThreshold = 0.025f;
const float kPreemptiveCrossfadeTime = 0.1f;

struct Parameters {
  RandomizedParameter time;
  RandomizedParameter size;
  RandomizedParameter shape;
  RandomizedParameter pitch;
  bool monophonic_mode;
  // Nebulae port: probability of playing a grain in reverse on top of the
  // short-grain reversal of Beads (0 = Beads behaviour), and extra random
  // panning of the grains (0 = Beads behaviour, 1 = uniform across the field).
  float reverse_probability;
  float stereo_spread;
};

template<typename T, int num_grains>
class GranularPlayer {
 public:
  GranularPlayer() { }
  ~GranularPlayer() { }
  
  enum {
    NUM_GRAINS = num_grains
  };
  
  inline void Init(float sample_rate) {
    instant_crossfade_time_ = int(sample_rate * kInstantCrossfadeTime);
    preemptive_crossfade_time_ = int(sample_rate * kPreemptiveCrossfadeTime);
    simple_scheduler_threshold_ = int(sample_rate * kSimpleSchedulerThreshold);
    
    grain_interval_ = 1;
    
    max_grain_duration_ = sample_rate * 4.0f;
    sample_rate_ = sample_rate;
    one_hertz_ = 1.0f / sample_rate;
    Reset();
  }
  
  inline void Reset() {
    for (int i = 0; i < num_grains; ++i) {
      grain_[i].Init(sample_rate_);
      grain_priority_list_[i] = &grain_[i];
    }
    max_grains_ = render_limit_ = num_grains;
  }

  // Lich port: limits the number of grains, as if the player had been built
  // with max_grains voices (at least 4, Beads' number of "safety" grains).
  // Grains above a lowered limit fade out like stolen grains.
  inline void set_max_grains(int max_grains) {
    if (max_grains < 4) max_grains = 4;
    if (max_grains > num_grains) max_grains = num_grains;
    if (max_grains == max_grains_) {
      return;
    }
    if (max_grains < max_grains_) {
      for (int i = max_grains; i < max_grains_; ++i) {
        grain_[i].FadeOut(instant_crossfade_time_);
      }
      // Keep the priority order of the remaining grains.
      int n = 0;
      for (int i = 0; i < max_grains_; ++i) {
        if (grain_priority_list_[i] < &grain_[max_grains]) {
          grain_priority_list_[n++] = grain_priority_list_[i];
        }
      }
    } else {
      // The new voices are idle: first in line to be used.
      const int added = max_grains - max_grains_;
      for (int i = max_grains_ - 1; i >= 0; --i) {
        grain_priority_list_[i + added] = grain_priority_list_[i];
      }
      for (int i = 0; i < added; ++i) {
        grain_priority_list_[i] = &grain_[max_grains_ + i];
      }
    }
    max_grains_ = max_grains;
    if (render_limit_ < max_grains_) render_limit_ = max_grains_;
  }

  inline int max_grains() const { return max_grains_; }

  // Grains to render: the active ones, plus those above a lowered limit
  // that are still fading out.
  inline int render_limit() {
    while (render_limit_ > max_grains_ && grain_[render_limit_ - 1].done()) {
      --render_limit_;
    }
    return render_limit_;
  }
  
  inline float ComputeSpread(
      const TriggerGenerator::Output& trigger,
      const Parameters& p) const {
    float total_spread = 0.0f;
    total_spread += p.time.randomness_amount();
    total_spread += p.size.randomness_amount();
    total_spread += p.shape.randomness_amount();
    total_spread += p.pitch.randomness_amount();
    total_spread += 1.0f - trigger.regularity;
    return std::min(total_spread, 1.0f);
  }
  
  inline float ComputeGrainDuration(float duration) {
    const bool reverse = duration < 0.3333333f;
    const float clamped_shortness = reverse
        ? duration * 3.0f * (2.0f - duration * 3.0f)
        : 1.0f - 1.5f * (duration - 0.3333333f);
    const float shortness = std::min(1.0f, 1.025f * clamped_shortness);
    return stmlib::SemitonesToRatio(shortness * -108.0f) * max_grain_duration_;
  }
  
  inline int CountActiveGrains() const {
    int num_active_grains = 0;
    for (int i = 0; i < num_grains; ++i) {
      if (!grain_[i].done()) {
        ++num_active_grains;
      }
    }
    return num_active_grains;
  }
  
  inline void ComputeGrainAmplitudeAndPan(
      const TriggerGenerator::Output& trigger,
      const Parameters& p,
      float* amplitude,
      float* pan) {
    const float num_active_grains = float(CountActiveGrains() + 1);
    *amplitude = 1.0f / stmlib::Sqrt(num_active_grains);

    const float total_spread = ComputeSpread(trigger, p);
    const float pan_spread = 0.5f * num_active_grains * total_spread;
    *pan = BoundedRandomFloat(pan_spread);
    if (p.stereo_spread > 0.0f) {
      *pan += p.stereo_spread * (stmlib::Random::GetFloat() - *pan);
    }
  }
  
  inline T* new_grain() { return grain_priority_list_[0]; }
  
  inline void Tick() {
    ++grain_interval_;
  }

  inline void Tick(int num_samples) {
    grain_interval_ += num_samples;
  }
  
  inline void SortGrainList(bool monophonic_mode) {
    // Whenever a new grain is started, we subject the first N grains to various
    // crossfades, so they terminate quickly in case a new grain is started
    // afterwards.

    if (monophonic_mode) {
      std::rotate(
          &grain_priority_list_[0],
          &grain_priority_list_[1],
          &grain_priority_list_[max_grains_]);
      grain_priority_list_[0]->FadeOut(
          std::min(grain_interval_, instant_crossfade_time_));
      for (int i = 1; i < max_grains_ - 1; ++i) {
        grain_priority_list_[i]->FadeOut(instant_crossfade_time_);
      }
    } else {
      const int num_safety_grains = 4;
      int fade_time[num_safety_grains];
    
      if (grain_interval_ < simple_scheduler_threshold_) {
        // When the grains are arriving very fast, we simply assume that the
        // first grain will be done or close to completion, and we steal it.
        //
        // The newly created grain is moved back at the end of the list.
        std::rotate(
            &grain_priority_list_[0],
            &grain_priority_list_[1],
            &grain_priority_list_[max_grains_]);
        
        fade_time[0] = grain_interval_ + 1;
        fade_time[1] = instant_crossfade_time_;
        fade_time[2] = instant_crossfade_time_;
        fade_time[3] = preemptive_crossfade_time_;
        
        if (grain_interval_ > instant_crossfade_time_) {
          std::swap(fade_time[0], fade_time[2]);
        }
      } else {
        // The grains are generated at a rate not too high to allow the grain
        // list to be resorted. The sorting criterion is how much time
        // is left for a grain to play. Thus, the grains at the beginning of the
        // list will be the best candidates to "steal" without causing a drastic
        // change in texture. If the overlap is not too high, they are very
        // likely to have already completed their course anyway.
        std::partial_sort(
            &grain_priority_list_[0],
            &grain_priority_list_[num_safety_grains],
            &grain_priority_list_[max_grains_],
            T::Compare);
      
        fade_time[0] = instant_crossfade_time_;
        fade_time[1] = instant_crossfade_time_;
        fade_time[2] = grain_interval_;
        fade_time[3] = preemptive_crossfade_time_;

        if (grain_interval_ > preemptive_crossfade_time_) {
          std::swap(fade_time[2], fade_time[3]);
        }
      }
    
      // We make sure the grains at the beginning of the list will terminate
      // quickly and that not much damage will happen when they are stolen.
      for (int i = 0; i < num_safety_grains; ++i) {
        grain_priority_list_[i]->FadeOut(fade_time[i]);
      }
    }
    grain_interval_ = 0;
  }
  
 protected:
  T grain_[num_grains];
  T* grain_priority_list_[num_grains];
  int max_grains_;
  int render_limit_;
 
  int preemptive_crossfade_time_;
  int instant_crossfade_time_;
  int simple_scheduler_threshold_;
  
  float max_grain_duration_;
  float sample_rate_;
  float one_hertz_;
  
  int grain_interval_;
  
  DISALLOW_COPY_AND_ASSIGN(GranularPlayer);
};

}  // namespace beads

#endif  // BEADS_DSP_GRANULAR_PLAYER_H_
