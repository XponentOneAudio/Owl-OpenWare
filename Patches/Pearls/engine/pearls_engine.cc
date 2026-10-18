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
// Pearls engine. The control flow follows beads/beads.cc (Process() and
// MonitorStateChange()) and the seed button logic of beads/ui.cc.

#include "engine/pearls_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

namespace pearls {

using namespace beads;
using namespace stmlib;
using namespace std;

namespace {

// Same memory layout as the Beads firmware: 512 kB + 256 kB of delay memory
// split in three 256 kB regions, and 64 kB of reverb memory.
const size_t kDelayRegionSize = 256 * 1024;
// Reverb memory: 32768 samples, half with the Lich's 24 kHz reverb.
const size_t kReverbBufferSize = beads::ReverbT<BEADS_REVERB_DIVIDER>::kBufferSize;

const float kSeedLongPressTime = 4.0f;      // s, seed button relocks
const float kSeedPatchTimeout = 4.0f;       // s without edges = unpatched
const size_t kSeedDelay = 120;              // 2.5 ms at 48 kHz, as Beads
const float kFadeOutTime = 0.01f;           // s, before reconfiguring
const float kFadeInTime = 0.05f;            // s, after reconfiguring
const float kSeedLedTime = 0.04f;           // s

// Tap tempo: taps more than kTapTimeout apart start a new measurement, an
// interval more than 30% away from the average restarts the average, and a
// press of kTapStopTime stops the clock.
const float kTapTimeout = 4.0f;             // s
const float kMinTapInterval = 0.05f;        // s
const float kTapStopTime = 1.0f;            // s
const float kClockPulseTime = 0.005f;       // s
const float kClockLedTime = 0.05f;          // s

// Frames copied to or from the buffer per Process() call: 4096 is about
// 0.2 ms of work on a Raspberry Pi 3; the Lich uses 256.
#ifndef PEARLS_TRANSFER_CHUNK
#define PEARLS_TRANSFER_CHUNK 4096
#endif
const size_t kTransferChunk = PEARLS_TRANSFER_CHUNK;

// Input presence detection, replacing the Beads normalisation probe: input
// is considered unpatched after 2 s below -66 dBFS, and patched again as soon
// as the level exceeds -60 dBFS.
const float kInputPresentThreshold = 0.001f;
const float kInputAbsentThreshold = 0.0005f;
const float kInputAbsentTime = 2.0f;        // s

// Intermediate rate for the 48k <-> 32k resamplers is 96 kHz.
const double kResamplerCutoff = 14500.0 / 96000.0;
const double kResamplerKaiserBeta = 7.0;

inline size_t Seconds(float t) {
  return size_t(t * float(Engine::kSampleRate));
}

GainControl::SoftClippingMode SoftClippingModeForQuality(int quality) {
  switch (quality) {
    case QUALITY_DIRTY_TAPE:
      return GainControl::SOFT_CLIPPING_MODE_ON;
    case QUALITY_CLEAN_TAPE:
      return GainControl::SOFT_CLIPPING_MODE_DYNAMIC;
    default:
      return GainControl::SOFT_CLIPPING_MODE_OFF;
  }
}

}  // namespace

uint16_t ScaleMask(int scale) {
  static const uint16_t masks[SCALE_LAST] = {
    0x000,  // off
    0xfff,  // chromatic
    0xab5,  // major: 0 2 4 5 7 9 11
    0x5ad,  // natural minor: 0 2 3 5 7 8 10
    0x295,  // major pentatonic: 0 2 4 7 9
    0x4a9,  // minor pentatonic: 0 3 5 7 10
    0x081,  // octaves and fifths: 0 7
    0x001,  // octaves
  };
  return scale >= 0 && scale < SCALE_LAST ? masks[scale] : 0;
}

void Controls::SetDefaults() {
  time = 0.5f;
  size = 0.5f;
  shape = 0.5f;
  density = 0.75f;
  pitch = 0.0f;
  time_random = size_random = shape_random = pitch_random = 0.0f;
  feedback = 0.0f;
  dry_wet = 0.5f;
  reverb = 0.0f;
  reverse = 0.0f;
  spread = 0.0f;
  scale = SCALE_OFF;
  quality = QUALITY_DIGITAL;
  mono = false;
  synth = false;
  freeze = false;
  seed_button = false;
  tap = false;
  gain = 0;
}

size_t Engine::MaxBufferFrames() {
  // The longest buffer: 8-bit mono (dirty tape).
  return kDelayBufferSizeBits / 8;
}

Engine::~Engine() {
  if (processor_) processor_->~Processor();
  if (synth_) synth_->~Synth();
  free(processor_);
  free(synth_);
  free(delay_buffer_);
  free(reverb_buffer_);
}

bool Engine::Init() {
  // malloc and placement new, so that a failed allocation (possible on the
  // Lich) is reported instead of assumed away.
  // On the Lich the heap fills its 56 kB of internal CCM first, in this
  // order: the processor (15 kB of grain state, accessed most), the 24 kHz
  // reverb's memory (32 kB), the synth (4 kB); the delay buffer goes to
  // SDRAM.
  void* processor_memory = malloc(sizeof(Processor));
  reverb_buffer_ = static_cast<uint16_t*>(
      malloc(kReverbBufferSize * sizeof(uint16_t)));
  void* synth_memory = malloc(sizeof(Synth));
  delay_buffer_ = static_cast<uint8_t*>(malloc(3 * kDelayRegionSize));
  if (!delay_buffer_ || !reverb_buffer_ || !processor_memory ||
      !synth_memory) {
    free(processor_memory);
    free(synth_memory);
    return false;
  }
  processor_ = new (processor_memory) Processor;
  synth_ = new (synth_memory) Synth;
  memset(delay_buffer_, 0, 3 * kDelayRegionSize);
  memset(reverb_buffer_, 0, kReverbBufferSize * sizeof(uint16_t));

  uint8_t* buffers[3] = {
    delay_buffer_,
    delay_buffer_ + kDelayRegionSize,
    delay_buffer_ + 2 * kDelayRegionSize
  };
  processor_->Init(buffers, reverb_buffer_);
  synth_->Init(reverb_buffer_);

  down_.Init(kResamplerCutoff, kResamplerKaiserBeta);
  up_.Init(kResamplerCutoff, kResamplerKaiserBeta);
  ResetStreaming();
  streaming_underflows_ = 0;

  controls_.SetDefaults();
  memset(&status_, 0, sizeof(status_));

  fifo_index_ = 0;
  fill(&in_fifo_l_[0], &in_fifo_l_[kBlockSize], 0.0f);
  fill(&in_fifo_r_[0], &in_fifo_r_[kBlockSize], 0.0f);
  fill(&out_fifo_l_[0], &out_fifo_l_[kBlockSize], 0.0f);
  fill(&out_fifo_r_[0], &out_fifo_r_[kBlockSize], 0.0f);
  fill(&freeze_fifo_[0], &freeze_fifo_[kBlockSize], false);
  fill(&seed_fifo_[0], &seed_fifo_[kBlockSize], false);
  fill(&gate_fifo_[0], &gate_fifo_[kBlockSize], false);

  synth_mode_ = controls_.synth;
  target_quality_ = controls_.quality;
  target_mono_ = controls_.mono;
  target_synth_ = controls_.synth;
  fade_ = 0.0f;
  reconfigure_pending_ = false;
  reset_gain_pending_ = true;
  gain_control_.Init(
      float(kSampleRate), SoftClippingModeForQuality(target_quality_), true);
  ApplyConfiguration();

  seed_locked_ = true;
  seed_pressed_ = false;
  seed_combo_ = false;
  seed_press_duration_ = 0;

  seed_gate_previous_ = false;
  seed_idle_time_ = 0;
  fill(&seed_delay_line_[0], &seed_delay_line_[128], false);
  seed_delay_write_ = 0;
  seed_flags_previous_ = GATE_FLAG_LOW;
  seed_flags_previous_32k_ = GATE_FLAG_LOW;

  input_envelope_ = 0.0f;
  input_silence_time_ = Seconds(kInputAbsentTime);
  status_.input_present = false;

  pitch_ = 0.0f;
  seed_led_time_ = 0;
  status_.seed_locked = true;
  status_.overwrite_reserve = 1.0f;

  freeze_inverted_ = false;
  freeze_button_previous_ = false;
  frozen_previous_block_ = false;
  configured_ = false;
  restart_gain_pending_ = false;

  tap_previous_ = false;
  tap_press_duration_ = 0;
  since_last_tap_ = Seconds(kTapTimeout);
  num_tap_intervals_ = 0;
  clock_running_ = false;
  clock_period_ = 0.0f;
  clock_phase_ = 0.0f;
  clock_pulse_remaining_ = 0;
  clock_led_time_ = 0;

  transfer_state_ = TRANSFER_IDLE;
  load_l_ = load_r_ = NULL;
  load_reader_ = NULL;
  load_context_ = NULL;
  save_l_ = save_r_ = NULL;
  transfer_source_frames_ = 0;
  transfer_source_rate_ = float(kSampleRate);
  transfer_hold_ = false;
  transfer_started_ = false;
  transfer_position_ = 0.0;
  transfer_frames_ = 0;
  transfer_target_frames_ = 0;
  transfer_rate_ = float(kSampleRate);
  transfer_mono_ = false;
  transfer_write_ptr_ = 0;
  buffer_generation_ = 0;
  return true;
}

void Engine::ReadLoadArrays(const void* context, size_t frame,
                            float* l, float* r) {
  const Engine* engine = static_cast<const Engine*>(context);
  *l = engine->load_l_[frame];
  *r = engine->load_r_[frame];
}

bool Engine::StartLoad(
    const float* l,
    const float* r,
    size_t frames,
    float sample_rate,
    bool hold) {
  if (!l || !r) {
    return false;
  }
  if (!StartLoad(&ReadLoadArrays, this, frames, sample_rate, hold)) {
    return false;
  }
  load_l_ = l;
  load_r_ = r;
  return true;
}

bool Engine::StartLoad(
    FrameReader reader,
    const void* context,
    size_t frames,
    float sample_rate,
    bool hold) {
  if (transfer_state_ != TRANSFER_IDLE || !reader || !frames ||
      sample_rate < 1000.0f) {
    return false;
  }
  load_reader_ = reader;
  load_context_ = context;
  transfer_source_frames_ = frames;
  transfer_source_rate_ = sample_rate;
  transfer_hold_ = hold;
  transfer_started_ = false;
  transfer_state_ = TRANSFER_LOADING;
  return true;
}

bool Engine::StartSave(float* l, float* r, size_t capacity) {
  if (transfer_state_ != TRANSFER_IDLE || !l || !r) {
    return false;
  }
  save_l_ = l;
  save_r_ = r;
  transfer_target_frames_ = capacity;
  transfer_frames_ = 0;
  transfer_started_ = false;
  transfer_state_ = TRANSFER_SAVING;
  return true;
}

void Engine::ContinueTransfer() {
  if (transfer_state_ == TRANSFER_LOADING) {
    // Wait for the output to be muted and the configuration to be stable:
    // reconfiguring clears the buffer.
    if (reconfigure_pending_ || fade_ > 0.0f) {
      return;
    }
    const float rate = processor_->buffer_sample_rate();
    const double step = double(transfer_source_rate_) / double(rate);
    if (!transfer_started_) {
      processor_->TrimBuffer();
      transfer_rate_ = rate;
      transfer_mono_ = processor_->mono();
      transfer_frames_ = 0;
      // Leave room for the interpolation tail: a full buffer would wrap.
      const size_t capacity = size_t(processor_->buffer_capacity()) - 8;
      const size_t converted = size_t(
          double(transfer_source_frames_ - 1) / step) + 1;
      transfer_target_frames_ = min(capacity, converted);
      transfer_started_ = true;
    }
    const size_t end = min(
        transfer_target_frames_, transfer_frames_ + kTransferChunk);
    const size_t last = transfer_source_frames_ - 1;
    for (; transfer_frames_ < end; ++transfer_frames_) {
      const double position = double(transfer_frames_) * step;
      const size_t i = min(size_t(position), last);
      FloatFrame frame;
      if (step > 1.0) {
        // Downsampling: average the source frames this frame covers.
        const size_t j = min(
            max(size_t(position + step), i + 1), last + 1);
        float l = 0.0f, r = 0.0f;
        for (size_t k = i; k < j; ++k) {
          float kl, kr;
          load_reader_(load_context_, k, &kl, &kr);
          l += kl;
          r += kr;
        }
        frame.l = l / float(j - i);
        frame.r = r / float(j - i);
      } else {
        // Upsampling: linear interpolation.
        const size_t next = min(i + 1, last);
        const float t = float(position - double(i));
        float il, ir, nl, nr;
        load_reader_(load_context_, i, &il, &ir);
        load_reader_(load_context_, next, &nl, &nr);
        frame.l = il + (nl - il) * t;
        frame.r = ir + (nr - ir) * t;
      }
      CONSTRAIN(frame.l, -1.0f, 1.0f);
      CONSTRAIN(frame.r, -1.0f, 1.0f);
      processor_->WriteBuffer(frame);
    }
    if (transfer_frames_ >= transfer_target_frames_) {
      // As if the FREEZE button had been pressed (Beads restores its
      // backed-up buffer that way): one press releases it.
      if (transfer_hold_ && !(controls_.freeze != freeze_inverted_)) {
        freeze_inverted_ = !freeze_inverted_;
      }
      ++buffer_generation_;
      transfer_state_ = TRANSFER_DONE;
    }
  } else if (transfer_state_ == TRANSFER_SAVING) {
    const int capacity = processor_->buffer_capacity();
    const int write_ptr = *processor_->mutable_delay_write_ptr();
    if (!transfer_started_) {
      transfer_rate_ = processor_->buffer_sample_rate();
      transfer_mono_ = processor_->mono();
      transfer_target_frames_ = min(
          transfer_target_frames_, size_t(processor_->buffer_duration()));
      transfer_write_ptr_ = write_ptr;
      transfer_frames_ = 0;
      transfer_started_ = true;
    }
    // Frames recorded since the save started shift the delays.
    const int written = (transfer_write_ptr_ - write_ptr + capacity) %
        capacity;
    const size_t end = min(
        transfer_target_frames_, transfer_frames_ + kTransferChunk);
    for (; transfer_frames_ < end; ++transfer_frames_) {
      int delay = int(transfer_target_frames_ - transfer_frames_) + written;
      if (delay > capacity) {
        delay = capacity;
      }
      const FloatFrame frame = processor_->ReadBuffer(delay);
      save_l_[transfer_frames_] = frame.l;
      save_r_[transfer_frames_] = frame.r;
    }
    if (transfer_frames_ >= transfer_target_frames_) {
      transfer_state_ = TRANSFER_DONE;
    }
  }
}

void Engine::UpdateTapClock(bool* pulses) {
  const bool tap = controls_.tap;
  if (tap && !tap_previous_) {
    tap_press_duration_ = 0;
    if (since_last_tap_ < Seconds(kTapTimeout)) {
      const float interval = float(since_last_tap_);
      if (interval >= float(Seconds(kMinTapInterval))) {
        float average = 0.0f;
        for (size_t i = 0; i < num_tap_intervals_; ++i) {
          average += tap_intervals_[i];
        }
        if (num_tap_intervals_) {
          average /= float(num_tap_intervals_);
          if (fabsf(interval - average) > 0.3f * average) {
            num_tap_intervals_ = 0;
          }
        }
        if (num_tap_intervals_ == 4) {
          copy(&tap_intervals_[1], &tap_intervals_[4], &tap_intervals_[0]);
          --num_tap_intervals_;
        }
        tap_intervals_[num_tap_intervals_++] = interval;
        average = 0.0f;
        for (size_t i = 0; i < num_tap_intervals_; ++i) {
          average += tap_intervals_[i];
        }
        clock_period_ = average / float(num_tap_intervals_);
        clock_running_ = true;
        clock_phase_ = 0.0f;
      }
    } else if (clock_running_) {
      // A single tap re-aligns the beat.
      clock_phase_ = 0.0f;
    }
    since_last_tap_ = 0;
  }
  if (tap) {
    tap_press_duration_ += kBlockSize;
    if (tap_press_duration_ >= Seconds(kTapStopTime)) {
      clock_running_ = false;
      num_tap_intervals_ = 0;
      since_last_tap_ = Seconds(kTapTimeout);
    }
  }
  tap_previous_ = tap;
  if (since_last_tap_ < Seconds(kTapTimeout)) {
    since_last_tap_ += kBlockSize;
  }

  const size_t pulse_length = min(
      Seconds(kClockPulseTime), size_t(clock_period_ * 0.5f));
  for (size_t i = 0; i < kBlockSize; ++i) {
    if (clock_running_) {
      if (clock_phase_ <= 0.0f) {
        clock_phase_ += clock_period_;
        clock_pulse_remaining_ = pulse_length;
        clock_led_time_ = Seconds(kClockLedTime);
      }
      clock_phase_ -= 1.0f;
    }
    pulses[i] = clock_pulse_remaining_ > 0;
    if (clock_pulse_remaining_) {
      --clock_pulse_remaining_;
    }
  }
  if (!clock_running_) {
    clock_led_time_ = 0;
  } else if (clock_led_time_ > kBlockSize) {
    clock_led_time_ -= kBlockSize;
  } else {
    clock_led_time_ = 0;
  }
}

void Engine::Process(
    const Controls& controls,
    const float* in_l,
    const float* in_r,
    const uint8_t* freeze_gate,
    const uint8_t* seed_gate,
    float* out_l,
    float* out_r,
    uint8_t* gate_out,
    size_t size) {
  controls_ = controls;
  ContinueTransfer();
  for (size_t i = 0; i < size; ++i) {
    // Inputs first: the output may be the input buffer (in-place hosts).
    in_fifo_l_[fifo_index_] = in_l ? in_l[i] : 0.0f;
    in_fifo_r_[fifo_index_] = in_r ? in_r[i] : 0.0f;
    out_l[i] = out_fifo_l_[fifo_index_];
    out_r[i] = out_fifo_r_[fifo_index_];
    if (gate_out) {
      gate_out[i] = gate_fifo_[fifo_index_] ? 1 : 0;
    }
    freeze_fifo_[fifo_index_] = freeze_gate ? freeze_gate[i] != 0 : false;
    seed_fifo_[fifo_index_] = seed_gate ? seed_gate[i] != 0 : false;
    if (++fifo_index_ == kBlockSize) {
      RenderBlock();
      fifo_index_ = 0;
    }
  }
}

void Engine::UpdateSeedButton(bool pressed, bool freeze_toggled) {
  // beads/ui.cc: pressing SEED unlocks the generator and seeds while held;
  // releasing it after a 4 s press locks it again. SEED + FREEZE locks it
  // too (and the FREEZE press is not a freeze toggle): seed_combo_ then
  // ignores SEED until it is released.
  if (pressed && !seed_pressed_) {
    seed_locked_ = false;
    seed_press_duration_ = 0;
    seed_combo_ = false;
  }
  if (pressed) {
    seed_press_duration_ += kBlockSize;
    if (freeze_toggled && !seed_combo_) {
      seed_locked_ = true;
      seed_combo_ = true;
      freeze_inverted_ = !freeze_inverted_;
    }
  } else if (seed_pressed_ && !seed_combo_ &&
             seed_press_duration_ >= Seconds(kSeedLongPressTime)) {
    seed_locked_ = true;
  }
  seed_pressed_ = pressed;
}

void Engine::UpdatePitch() {
  // The knob/CV pitch snaps to the scale too (it is also the root of the
  // synth's oscillator and the delay's pitch shifter). A note changes only
  // once the pitch is 0.2 semitone past the midpoint, so that CV noise
  // doesn't flip between two notes.
  const uint16_t mask = ScaleMask(controls_.scale);
  const float pitch = controls_.pitch;
  if (!mask) {
    pitch_ = pitch;
    return;
  }
  const float candidate = QuantizeSemitones(pitch, mask);
  if (QuantizeSemitones(pitch_, mask) != pitch_ ||
      fabsf(pitch - candidate) + 0.2f < fabsf(pitch - pitch_)) {
    pitch_ = candidate;
  }
}

void Engine::RestartGainControl() {
  restart_gain_pending_ = true;
}

void Engine::UpdateConfiguration() {
  int quality = controls_.quality;
  CONSTRAIN(quality, 0, QUALITY_LAST - 1);
  const bool mono = controls_.mono;
  const bool synth = controls_.synth;

  if (!configured_) {
    // The first settings take effect at once (e.g. a saved quality setting),
    // even if the buffer starts frozen.
    configured_ = true;
    if (synth != synth_mode_ || Processor::Media(quality) !=
        processor_->media() || mono != processor_->mono()) {
      target_quality_ = quality;
      target_mono_ = mono;
      target_synth_ = synth;
      reset_gain_pending_ = true;
      ApplyConfiguration();
    }
  }

  // As Beads: quality, mono and synth mode changes wait while the buffer is
  // frozen (they would clear it).
  const bool deferred = frozen_previous_block_ && !reconfigure_pending_;

  // In synth mode the quality setting only selects the wavetable resolution,
  // which the synth reads directly: no reconfiguration needed (as Beads).
  const bool needs_reconfiguration = synth != synth_mode_ || (!synth && (
      Processor::Media(quality) != processor_->media() ||
      mono != processor_->mono()));

  if (deferred) {
    // Nothing to reconfigure for now.
  } else if (needs_reconfiguration) {
    if (!reconfigure_pending_) {
      reset_gain_pending_ = mono != processor_->mono() || synth_mode_;
    }
    reconfigure_pending_ = true;
    target_quality_ = quality;
    target_mono_ = mono;
    target_synth_ = synth;
  } else {
    reconfigure_pending_ = false;
  }

  if (reconfigure_pending_ || transfer_state_ == TRANSFER_LOADING) {
    // Mute while reconfiguring or loading the buffer.
    fade_ -= float(kBlockSize) / (kFadeOutTime * kSampleRate);
    if (fade_ <= 0.0f) {
      fade_ = 0.0f;
      if (reconfigure_pending_) {
        ApplyConfiguration();
        reconfigure_pending_ = false;
      }
    }
  } else {
    fade_ += float(kBlockSize) / (kFadeInTime * kSampleRate);
    if (fade_ > 1.0f) {
      fade_ = 1.0f;
    }
  }
}

void Engine::ApplyConfiguration() {
  synth_mode_ = target_synth_;
  if (!synth_mode_) {
    processor_->Configure(Processor::Media(target_quality_), target_mono_);
    down_.Reset();
    up_.Reset();
    ResetStreaming();
  }
  gain_control_.Init(
      float(kSampleRate),
      SoftClippingModeForQuality(target_quality_),
      reset_gain_pending_);
  reset_gain_pending_ = false;
  seed_flags_previous_32k_ = GATE_FLAG_LOW;
  // The processor cleared its buffer: start transfers again.
  transfer_started_ = false;
  ++buffer_generation_;
}

void Engine::DetectInput(const FloatFrame* in, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    const float peak = max(fabsf(in[i].l), fabsf(in[i].r));
    input_envelope_ = peak > input_envelope_
        ? peak
        : input_envelope_ * 0.9995f;
  }
  if (input_envelope_ > kInputPresentThreshold) {
    status_.input_present = true;
    input_silence_time_ = 0;
  } else if (input_envelope_ < kInputAbsentThreshold) {
    input_silence_time_ += size;
    if (input_silence_time_ >= Seconds(kInputAbsentTime)) {
      input_silence_time_ = Seconds(kInputAbsentTime);
      status_.input_present = false;
    }
  }
}

void Engine::BuildParameters(
    Parameters* p,
    ModulatedParameter* d,
    Processor::MixParameters* m) const {
  const Controls& c = controls_;

  // With no CV "patched" (Nebulae sums CVs into the control values before
  // they reach us), the attenuverters set the amount of internal randomness.
  p->time.base = c.time;
  p->time.attenuverter = c.time_random;
  p->time.cv = 0.0f;
  p->time.has_cv = false;

  p->size.base = c.size;
  p->size.attenuverter = c.size_random;
  p->size.cv = 0.0f;
  p->size.has_cv = false;

  p->shape.base = c.shape;
  p->shape.attenuverter = c.shape_random;
  p->shape.cv = 0.0f;
  p->shape.has_cv = false;

  p->pitch.base = pitch_;
  p->pitch.attenuverter = c.pitch_random;
  p->pitch.cv = 0.0f;
  p->pitch.has_cv = false;
  p->pitch.scale_mask = ScaleMask(c.scale);
  p->time.scale_mask = p->size.scale_mask = p->shape.scale_mask = 0;

  p->monophonic_mode = false;

  d->base = c.density;
  d->cv = 0.0f;

  m->feedback = c.feedback;
  m->dry_wet = c.dry_wet;
  m->reverb_amount = c.reverb;
  CONSTRAIN(m->feedback, 0.0f, 1.0f);
  CONSTRAIN(m->dry_wet, 0.0f, 1.0f);
  CONSTRAIN(m->reverb_amount, 0.0f, 1.0f);
  m->output_gate = false;

  p->reverse_probability = c.reverse;
  p->stereo_spread = c.spread;
  CONSTRAIN(p->reverse_probability, 0.0f, 1.0f);
  CONSTRAIN(p->stereo_spread, 0.0f, 1.0f);
}

void Engine::ProcessAudio(
    SeedButtonState seed_state,
    const GateFlags* seed,
    const bool* freeze,
    const ModulatedParameter& d,
    const Parameters& p,
    const Processor::MixParameters& m,
    FloatFrame* in,
    FloatFrame* out,
    bool* gate) {
  processor_->set_max_grains(max_grains_);
  if (processor_->sample_rate() != 32000.0f) {
    processor_->set_gate_output(gate);
    processor_->Process(seed_state, seed, freeze, d, p, m, in, out, kBlockSize);
    return;
  }

  if (kStreaming32k) {
    ProcessAudio32kStreaming(seed_state, seed, freeze, d, p, m, in, out, gate);
    return;
  }

  // Vintage digital: run the processor at 32 kHz, 8 frames per block.
  const size_t n = kBlockSize * 2 / 3;
  float l[kBlockSize], r[kBlockSize];
  float l32[n], r32[n];
  for (size_t i = 0; i < kBlockSize; ++i) {
    l[i] = in[i].l;
    r[i] = in[i].r;
  }
  down_.Process(l, r, l32, r32, kBlockSize);

  FloatFrame in32[n], out32[n];
  bool freeze32[n];
  bool gate32[n];
  GateFlags seed32[n];
  for (size_t j = 0; j < n; ++j) {
    in32[j].l = l32[j];
    in32[j].r = r32[j];
    // 48 kHz frames covered by this 32 kHz frame.
    const size_t first = j * kBlockSize / n;
    const size_t last = (j + 1) * kBlockSize / n;
    bool f = false;
    bool s = false;
    for (size_t i = first; i < last; ++i) {
      f = f || freeze[i];
      if (seed) {
        s = s || (seed[i] & GATE_FLAG_HIGH);
      }
    }
    freeze32[j] = f;
    seed32[j] = seed_flags_previous_32k_ = ExtractGateFlags(
        seed_flags_previous_32k_, s);
  }
  if (!seed) {
    seed_flags_previous_32k_ = GATE_FLAG_LOW;
  }

  processor_->set_gate_output(gate32);
  processor_->Process(
      seed_state, seed ? seed32 : NULL, freeze32, d, p, m, in32, out32, n);

  for (size_t j = 0; j < n; ++j) {
    l32[j] = out32[j].l;
    r32[j] = out32[j].r;
  }
  up_.Process(l32, r32, l, r, n);
  for (size_t i = 0; i < kBlockSize; ++i) {
    out[i].l = l[i];
    out[i].r = r[i];
    gate[i] = gate32[i * n / kBlockSize];
  }
}

void Engine::ResetStreaming() {
  pending32_ = 0;
  // A few frames of latency, so that the varying number of 48 kHz frames
  // each 32 kHz block gives back always covers a block.
  out48_count_ = kStreamingLatency;
  fill(&out48_[0], &out48_[kOutFifoSize], FloatFrame());
  fill(&out48_gate_[0], &out48_gate_[kOutFifoSize], false);
}

void Engine::ProcessAudio32kStreaming(
    SeedButtonState seed_state,
    const GateFlags* seed,
    const bool* freeze,
    const ModulatedParameter& d,
    const Parameters& p,
    const Processor::MixParameters& m,
    FloatFrame* in,
    FloatFrame* out,
    bool* gate) {
  // 48 kHz -> 32 kHz. Output j of this block falls at (offset + 3 j) / 2
  // on the 48 kHz input: the freeze and seed gates of the 48 kHz frames up
  // to the next output go with it.
  float l[kBlockSize], r[kBlockSize];
  for (size_t i = 0; i < kBlockSize; ++i) {
    l[i] = in[i].l;
    r[i] = in[i].r;
  }
  const size_t offset = down_.offset();
  float l32[kMax32kBlock], r32[kMax32kBlock];
  const size_t n = down_.ProcessStreaming(l, r, l32, r32, kBlockSize);
  for (size_t j = 0; j < n; ++j) {
    const size_t first = j == 0 ? 0 : (offset + 3 * j) / 2;
    const size_t last = j + 1 == n ? kBlockSize : (offset + 3 * (j + 1)) / 2;
    bool f = false;
    bool s = false;
    for (size_t i = first; i < last; ++i) {
      f = f || freeze[i];
      if (seed) {
        s = s || (seed[i] & GATE_FLAG_HIGH);
      }
    }
    const size_t k = pending32_ + j;
    pending_in32_[k].l = l32[j];
    pending_in32_[k].r = r32[j];
    pending_freeze32_[k] = f;
    pending_seed32_[k] = seed_flags_previous_32k_ = ExtractGateFlags(
        seed_flags_previous_32k_, s);
  }
  if (!seed) {
    seed_flags_previous_32k_ = GATE_FLAG_LOW;
  }
  pending32_ += n;

  // The processor, on an even number of frames.
  const size_t count = pending32_ & ~size_t(1);
  FloatFrame out32[kMax32kBlock];
  bool gate32[kMax32kBlock];
  processor_->set_gate_output(gate32);
  processor_->Process(
      seed_state, seed ? pending_seed32_ : NULL, pending_freeze32_, d, p, m,
      pending_in32_, out32, count);
  if (pending32_ > count) {
    pending_in32_[0] = pending_in32_[count];
    pending_freeze32_[0] = pending_freeze32_[count];
    pending_seed32_[0] = pending_seed32_[count];
  }
  pending32_ -= count;

  // 32 kHz -> 48 kHz, into the output FIFO. Output k falls on input
  // (offset + 2 k) / 3, whose gate it takes.
  for (size_t j = 0; j < count; ++j) {
    l32[j] = out32[j].l;
    r32[j] = out32[j].r;
  }
  const size_t up_offset = up_.offset();
  float l48[kOutFifoSize], r48[kOutFifoSize];
  const size_t produced = up_.ProcessStreaming(l32, r32, l48, r48, count);
  for (size_t k = 0; k < produced && out48_count_ < kOutFifoSize; ++k) {
    out48_[out48_count_].l = l48[k];
    out48_[out48_count_].r = r48[k];
    out48_gate_[out48_count_] = count ? gate32[std::min(
        (up_offset + 2 * k) / 3, count - 1)] : false;
    ++out48_count_;
  }

  // One block out.
  const size_t available = std::min(out48_count_, size_t(kBlockSize));
  if (available < kBlockSize) {
    ++streaming_underflows_;
  }
  for (size_t i = 0; i < kBlockSize; ++i) {
    out[i] = i < available ? out48_[i] : FloatFrame();
    gate[i] = i < available ? out48_gate_[i] : false;
  }
  copy(&out48_[available], &out48_[out48_count_], &out48_[0]);
  copy(&out48_gate_[available], &out48_gate_[out48_count_], &out48_gate_[0]);
  out48_count_ -= available;
}

void Engine::RenderBlock() {
  UpdateConfiguration();
  UpdatePitch();

  FloatFrame in[kBlockSize];
  FloatFrame out[kBlockSize];
  bool freeze[kBlockSize];
  GateFlags seed[kBlockSize];
  bool gate[kBlockSize];

  // Freeze: the button (whose state a load, a restore or the SEED + FREEZE
  // combo can invert), the gate, and always while loading so that the
  // input is not recorded over the loaded frames.
  const bool freeze_toggled = controls_.freeze != freeze_button_previous_;
  freeze_button_previous_ = controls_.freeze;
  UpdateSeedButton(controls_.seed_button, freeze_toggled);
  const bool freeze_button = controls_.freeze != freeze_inverted_;
  const bool loading = transfer_state_ == TRANSFER_LOADING;

  bool freeze_block = false;
  bool recorded = false;
  for (size_t i = 0; i < kBlockSize; ++i) {
    in[i].l = in_fifo_l_[i];
    in[i].r = in_fifo_r_[i];
    freeze[i] = freeze_button || loading || freeze_fifo_[i];
    freeze_block = freeze_block || freeze[i];
    recorded = recorded || !freeze[i];
    gate[i] = false;
  }
  if (recorded && !synth_mode_) {
    ++buffer_generation_;
  }

  bool clock[kBlockSize];
  UpdateTapClock(clock);

  // Seed gate. Beads only uses its seed input when a cable is plugged in;
  // here, an edge marks the input as patched until it stays idle for a while.
  // As on Beads, the gate is delayed by 2.5 ms so that a CV changing at the
  // same time as the trigger is taken into account.
  for (size_t i = 0; i < kBlockSize; ++i) {
    const bool g = seed_fifo_[i] || clock[i];
    if (g) {
      if (!seed_gate_previous_) {
        status_.seed_input_patched = true;
      }
      seed_idle_time_ = 0;
    }
    seed_gate_previous_ = g;
    seed_delay_line_[seed_delay_write_] = g;
    const bool delayed = seed_delay_line_[
        (seed_delay_write_ + 128 - kSeedDelay) % 128];
    seed_delay_write_ = (seed_delay_write_ + 1) % 128;
    seed[i] = seed_flags_previous_ = ExtractGateFlags(
        seed_flags_previous_, delayed);
  }
  seed_idle_time_ += kBlockSize;
  if (seed_idle_time_ >= Seconds(kSeedPatchTimeout)) {
    seed_idle_time_ = Seconds(kSeedPatchTimeout);
    status_.seed_input_patched = false;
  }
  const GateFlags* seed_input = status_.seed_input_patched ? seed : NULL;

  const SeedButtonState seed_state = seed_locked_
      ? SEED_BUTTON_STATE_LOCKED
      : (seed_pressed_ && !seed_combo_
            ? SEED_BUTTON_STATE_ON
            : SEED_BUTTON_STATE_OFF);

  Parameters p;
  ModulatedParameter d;
  Processor::MixParameters m;
  BuildParameters(&p, &d, &m);

  if (synth_mode_) {
    int quality = controls_.quality;
    CONSTRAIN(quality, 0, QUALITY_LAST - 1);
    synth_->set_gate_output(gate);
    synth_->Render(
        seed_state, seed_input, freeze,
        Synth::Resolution(quality),
        d, p, m, out, kBlockSize);
  } else {
    DetectInput(in, kBlockSize);
    const bool absent = !status_.input_present;
    if (restart_gain_pending_) {
      restart_gain_pending_ = false;
      gain_control_.Init(float(kSampleRate),
                         SoftClippingModeForQuality(target_quality_), true);
    }
    gain_control_.Process(
        fade_ >= 1.0f,
        controls_.gain,
        absent,
        absent || controls_.mono,
        in, in, kBlockSize);
    ProcessAudio(seed_state, seed_input, freeze, d, p, m, in, out, gate);
  }

  // Fade around reconfigurations, then hand over to the output FIFO.
  bool any_gate = false;
  for (size_t i = 0; i < kBlockSize; ++i) {
    float l = out[i].l * fade_;
    float r = out[i].r * fade_;
    CONSTRAIN(l, -1.0f, 1.0f);
    CONSTRAIN(r, -1.0f, 1.0f);
    out_fifo_l_[i] = l;
    out_fifo_r_[i] = r;
    gate_fifo_[i] = gate[i];
    any_gate = any_gate || gate[i];
  }

  if (any_gate || (seed_pressed_ && !seed_locked_)) {
    seed_led_time_ = Seconds(kSeedLedTime);
  } else if (seed_led_time_ > kBlockSize) {
    seed_led_time_ -= kBlockSize;
  } else {
    seed_led_time_ = 0;
  }

  status_.seed_locked = seed_locked_;
  status_.seed_led = seed_led_time_ > 0;
  status_.frozen = freeze_block;
  status_.latched_frozen = freeze_button;
  status_.gain_adjusting = !synth_mode_ && gain_control_.adjusting();
  frozen_previous_block_ = freeze_block;
  status_.clock_running = clock_running_;
  status_.clock_led = clock_led_time_ > 0;
  status_.overwrite_reserve = synth_mode_
      ? 1.0f
      : processor_->overwrite_reserve();
  status_.vu_meter = gain_control_.vu_meter();
}

}  // namespace pearls
