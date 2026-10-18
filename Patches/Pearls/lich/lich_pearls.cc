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
// Pearls on the Befaco Lich: panel and CPU governor.

#include "lich/lich_pearls.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

#include "engine/pearls_engine.h"
#include "beads/resources.h"

// Lich port: see owl/lich_config.h.
#ifndef PEARLS_COLD
#define PEARLS_COLD
#endif

namespace pearls {
namespace lich {

namespace {

// Pickup: a knob takes over its parameter when it comes this close to the
// parameter's value, or crosses it.
const float kPickupWindow = 0.02f;

// Button gestures, in seconds.
const float kComboHoldTime = 0.25f;   // SEED held this long, then FREEZE
const float kMomentaryTime = 0.6f;    // FREEZE held longer: release unfreezes
const float kLockHoldTime = 4.0f;     // SEED held this long, then released

// LEDs, in seconds.
const float kBlinkOn = 0.1f;
const float kBlinkPeriod = 0.25f;
const float kBeaconOn = 0.06f;
const float kBeaconPeriod = 0.18f;
const float kBeaconRepeat = 2.0f;
const float kGrainLedTime = 0.03f;
const int kDimDivider = 8;            // dim LED: on one block in 8

// Governor.
const float kLoadHigh = 0.85f;
const float kLoadCritical = 0.95f;
const float kLoadLow = 0.70f;
const float kDecreaseInterval = 0.01f;
const float kIncreaseInterval = 0.1f;
const int kMinGrains = 4;
const int kMaxGrains = 32;

// Setup page zones.
const int kNumQualityZones = 4;

// Selector knobs: n zones, with hysteresis around the current one.
inline int Zone(float v, int n, int current) {
  const float z = v * float(n);
  if (z < float(current) - 0.1f || z > float(current + 1) + 0.1f) {
    current = std::min(n - 1, std::max(0, int(z)));
  }
  return current;
}

inline float Bipolar(float v) {
  // Attenuverter with a dead zone at noon: -1..+1.
  float x = 2.0f * v - 1.0f;
  const float kDeadZone = 0.04f;
  if (fabsf(x) < kDeadZone) {
    return 0.0f;
  }
  x = x > 0.0f ? (x - kDeadZone) : (x + kDeadZone);
  return x / (1.0f - kDeadZone);
}

}  // namespace

const size_t LichPearls::kMaxBlockSize;
const int LichPearls::kDefaultMaxGrains;

LichPearls::LichPearls() : engine_(NULL), controls_(NULL) { }

LichPearls::~LichPearls() {
  if (engine_) {
    engine_->~Engine();
    free(engine_);
  }
  free(controls_);
}

PEARLS_COLD bool LichPearls::Init(float sample_rate) {
  sample_rate_ = sample_rate;
  // malloc and placement new: OWL patches have no nothrow operator new.
  void* engine_memory = malloc(sizeof(Engine));
  void* controls_memory = malloc(sizeof(Controls));
  if (!engine_memory || !controls_memory) {
    free(engine_memory);
    free(controls_memory);
    return false;
  }
  engine_ = new (engine_memory) Engine;
  controls_ = new (controls_memory) Controls;
  if (!engine_->Init()) {
    return false;
  }
  controls_->SetDefaults();
  // Beads' power-on state: no reverb, half dry/wet, square-ish envelope.
  const float defaults[PAGE_LAST][NUM_KNOBS] = {
    { 0.5f, 0.5f, 0.75f, 0.5f },     // TIME, SIZE, DENSITY, PITCH (unison)
    { 0.5f, 0.0f, 0.5f, 0.0f },      // SHAPE, FEEDBACK, DRY/WET, REVERB
    { 0.5f, 0.5f, 0.5f, 0.5f },      // no randomness
    { 0.0f, 0.0f, 0.0625f, 0.055f }, // as Beads: no reverse, spread, scale;
                                     // no sample
    { 0.125f, 0.0f, 0.15f, 0.15f },  // hi-fi, auto gain, stereo, button
  };
  for (int p = 0; p < PAGE_LAST; ++p) {
    for (int k = 0; k < NUM_KNOBS; ++k) {
      knobs_[p][k].value = defaults[p][k];
      knobs_[p][k].caught = false;
      knobs_[p][k].last_raw = -1.0f;
    }
  }
  // The main page follows the knobs from the start.
  page_ = PAGE_GRAINS;
  for (int k = 0; k < NUM_KNOBS; ++k) {
    knobs_[PAGE_GRAINS][k].caught = true;
    raw_[k] = defaults[PAGE_GRAINS][k];
  }

  for (int b = 0; b < NUM_BUTTONS; ++b) {
    button_[b] = false;
    held_time_[b] = 0;
  }
  freeze_latched_ = false;
  freeze_momentary_ = false;
  freeze_combo_ = false;
  seed_combo_ = false;
  seed_mode_ = SEED_MODE_BUTTON;
  seed_pulse_ = false;
  std::fill(&seed_gate_[0], &seed_gate_[kMaxBlockSize], 0);

  time_ = 0;
  blink_start_ = 0;
  blink_count_ = 0;
  grain_led_time_ = 0;
  last_quality_zone_ = 0;
  last_scale_zone_ = 0;
  last_sample_zone_ = 0;
  fast_blink_ = false;
  synth_zone_previous_ = false;
  sample_reading_ = false;
  sample_request_ = 0;
  sample_busy_ = false;
  memset(&wav_info_, 0, sizeof(wav_info_));

  random_cv_ = 0.5f;
  envelope_ = 0.0f;
  gate_previous_ = false;
  rng_ = 0x2545f491;

  load_ = 0.0f;
  load_peak_ = 0.0f;
  since_change_ = 0;
  grain_ceiling_ = kMaxGrains;
  max_grains_ = kDefaultMaxGrains;
  load_reported_ = false;
  engine_->set_max_grains(max_grains_);

#ifdef BEADS_EXTERNAL_WAVETABLES
  synth_available_ = beads::wav_integrated_waves != NULL;
#else
  synth_available_ = true;
#endif  // BEADS_EXTERNAL_WAVETABLES

  ui_.page = page_;
  ui_.frozen = false;
  ui_.seed_locked = true;
  ui_.max_grains = max_grains_;
  ui_.load = 0.0f;
  return true;
}

bool LichPearls::set_wavetables(const void* data, size_t bytes) {
#ifdef BEADS_EXTERNAL_WAVETABLES
  // tools/make_wavetable_resource.py prints this checksum (FNV-1a).
  const uint32_t kChecksum = 0xe2a09022;
  const size_t kBytes = WAV_INTEGRATED_WAVES_SIZE * sizeof(int16_t);
  synth_available_ = false;
  beads::wav_integrated_waves = NULL;
  if (!data || bytes < kBytes) {
    return false;
  }
  uint32_t h = 0x811c9dc5;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < kBytes; ++i) {
    h = (h ^ p[i]) * 0x01000193;
  }
  if (h != kChecksum) {
    return false;
  }
  beads::wav_integrated_waves = static_cast<const int16_t*>(data);
  synth_available_ = true;
  return true;
#else
  (void)data;
  (void)bytes;
  return true;  // built in
#endif  // BEADS_EXTERNAL_WAVETABLES
}

int LichPearls::TakeSampleRequest() {
  if (sample_busy_ || sample_reading_) {
    return 0;  // asked again when the current load is done
  }
  const int slot = sample_request_;
  sample_request_ = 0;
  return slot;
}

void LichPearls::SampleReading() {
  sample_reading_ = true;
}

PEARLS_COLD bool LichPearls::LoadSample(const void* wav, size_t bytes) {
  sample_reading_ = false;
  if (!ParseWav(wav, bytes, &wav_info_) ||
      !engine_->StartLoad(&ReadWavFrame, &wav_info_, wav_info_.frames,
                          wav_info_.sample_rate, true)) {
    SampleFailed();
    return false;
  }
  sample_busy_ = true;
  return true;
}

void LichPearls::SampleFailed() {
  sample_reading_ = false;
  Blink(6, true);
}

const Controls& LichPearls::controls() const {
  return *controls_;
}

void LichPearls::set_grain_ceiling(int grains) {
  grain_ceiling_ = std::max(kMinGrains, std::min(kMaxGrains, grains));
  if (max_grains_ > grain_ceiling_) {
    max_grains_ = grain_ceiling_;
  }
}

void LichPearls::ReportLoad(float load) {
  load_reported_ = true;
  load_ += 0.05f * (load - load_);
  load_peak_ = std::max(load, load_peak_ * 0.98f);
}

void LichPearls::UpdateGovernor(size_t size) {
  since_change_ += size;
  if (!load_reported_) {
    max_grains_ = std::min(kDefaultMaxGrains, grain_ceiling_);
  } else if (load_peak_ > kLoadHigh &&
             since_change_ >= size_t(kDecreaseInterval * sample_rate_)) {
    max_grains_ -= load_peak_ > kLoadCritical ? 2 : 1;
    since_change_ = 0;
  } else if (load_peak_ < kLoadLow &&
             since_change_ >= size_t(kIncreaseInterval * sample_rate_)) {
    ++max_grains_;
    since_change_ = 0;
  }
  max_grains_ = std::max(kMinGrains, std::min(grain_ceiling_, max_grains_));
  engine_->set_max_grains(max_grains_);
  ui_.max_grains = max_grains_;
  ui_.load = load_;
}

void LichPearls::Blink(int count, bool fast) {
  blink_start_ = time_;
  blink_count_ = count;
  fast_blink_ = fast;
}

void LichPearls::ChangePage(int page) {
  page_ = page;
  for (int k = 0; k < NUM_KNOBS; ++k) {
    knobs_[page_][k].caught = false;
    knobs_[page_][k].last_raw = raw_[k];
  }
  Blink(page_ + 1, false);
}

void LichPearls::ReadKnobs(const Inputs& inputs) {
  for (int k = 0; k < NUM_KNOBS; ++k) {
    float raw = inputs.knob[k];
    CONSTRAIN(raw, 0.0f, 1.0f);
    raw_[k] = raw;
    KnobState* s = &knobs_[page_][k];
    if (!s->caught) {
      const float d = raw - s->value;
      const bool crossed = s->last_raw >= 0.0f &&
          ((s->last_raw - s->value) * d <= 0.0f);
      if (fabsf(d) < kPickupWindow || crossed) {
        s->caught = true;
      }
      s->last_raw = raw;
    }
    if (s->caught) {
      s->value = raw;
    }
    ui_.knob_caught[k] = s->caught;
  }
}

void LichPearls::UpdateButtons(const Inputs& inputs, size_t size) {
  const size_t combo_hold = size_t(kComboHoldTime * sample_rate_);
  const size_t momentary = size_t(kMomentaryTime * sample_rate_);
  const size_t lock_hold = size_t(kLockHoldTime * sample_rate_);

  // SEED. In button mode it is the Beads SEED button (the engine handles a
  // 4 s press); in clock mode, the Beads SEED input, frame by frame.
  const bool seed = inputs.button[BUTTON_SEED];
  int edge = inputs.edge_frame[BUTTON_SEED];
  if (seed == button_[BUTTON_SEED]) {
    edge = -1;
  }
  const size_t from = edge < 0 ? 0 : std::min(size_t(edge), size);
  for (size_t i = 0; i < size; ++i) {
    seed_gate_[i] = (i < from && edge >= 0) ? button_[BUTTON_SEED] : seed;
  }
  if (seed && !button_[BUTTON_SEED]) {
    held_time_[BUTTON_SEED] = 0;
    seed_combo_ = false;
  }
  if (seed) {
    held_time_[BUTTON_SEED] += size;
  } else if (button_[BUTTON_SEED] && seed_mode_ == SEED_MODE_CLOCK) {
    // Released: a long press (not used for a page change) toggles the lock.
    if (!seed_combo_ && held_time_[BUTTON_SEED] >= lock_hold) {
      const bool locked = !engine_->status().seed_locked;
      engine_->set_seed_locked(locked);
      Blink(locked ? 1 : 2, false);
    }
  }

  // FREEZE: a press toggles; a long press is momentary. With SEED held, a
  // press changes the page instead.
  const bool freeze = inputs.button[BUTTON_FREEZE];
  if (freeze && !button_[BUTTON_FREEZE]) {
    held_time_[BUTTON_FREEZE] = 0;
    if (seed && held_time_[BUTTON_SEED] >= combo_hold) {
      freeze_combo_ = true;
      seed_combo_ = true;
      ChangePage((page_ + 1) % PAGE_LAST);
      // As Beads' SEED + FREEZE, the gesture locks the generator.
      engine_->set_seed_locked(true);
    } else {
      freeze_combo_ = false;
      freeze_momentary_ = false;
      freeze_latched_ = !freeze_latched_;
    }
  }
  if (freeze) {
    held_time_[BUTTON_FREEZE] += size;
    if (!freeze_combo_ && held_time_[BUTTON_FREEZE] >= momentary) {
      freeze_momentary_ = true;
    }
  } else if (button_[BUTTON_FREEZE]) {
    if (!freeze_combo_ && freeze_momentary_) {
      freeze_latched_ = !freeze_latched_;
    }
    freeze_combo_ = false;
    freeze_momentary_ = false;
  }

  button_[BUTTON_FREEZE] = freeze;
  button_[BUTTON_SEED] = seed;
}

void LichPearls::UpdateControls() {
  Controls& c = *controls_;
  const KnobState* g = knobs_[PAGE_GRAINS];
  c.time = g[KNOB_A].value;
  c.size = g[KNOB_B].value;
  c.density = g[KNOB_C].value;
  // 1 V/oct: the 10 V range spans 120 semitones, unison at noon.
  c.pitch = (g[KNOB_D].value - 0.5f) * 120.0f;
  CONSTRAIN(c.pitch, -48.0f, 48.0f);

  const KnobState* m = knobs_[PAGE_MIX];
  c.shape = m[KNOB_A].value;
  c.feedback = m[KNOB_B].value;
  c.dry_wet = m[KNOB_C].value;
  c.reverb = m[KNOB_D].value;

  const KnobState* r = knobs_[PAGE_RANDOM];
  c.time_random = Bipolar(r[KNOB_A].value);
  c.size_random = Bipolar(r[KNOB_B].value);
  c.shape_random = Bipolar(r[KNOB_C].value);
  c.pitch_random = Bipolar(r[KNOB_D].value);

  const KnobState* s = knobs_[PAGE_SETUP];
  // Quality, clockwise from hi-fi to scorched cassette.
  const int zone = Zone(s[KNOB_A].value, kNumQualityZones, last_quality_zone_);
  if (zone != last_quality_zone_) {
    last_quality_zone_ = zone;
    Blink(zone + 1, false);
  }
  c.quality = QUALITY_DIGITAL - zone;
#ifdef PEARLS_TEST_QUALITY
  c.quality = PEARLS_TEST_QUALITY;  // tools/owlhost tests
#endif
  const float gain = s[KNOB_B].value;
  c.gain = gain < 0.05f
      ? 0
      : uint8_t(1.0f + (gain - 0.05f) / 0.95f * 254.0f);
  // Buffer: stereo, mono, or the wavetable synth (which needs the tables).
  const float buffer = s[KNOB_C].value;
  c.mono = buffer >= 1.0f / 3.0f && buffer < 2.0f / 3.0f;
  const bool synth_zone = buffer >= 2.0f / 3.0f;
  c.synth = synth_available_ && synth_zone;
  if (synth_zone && !synth_zone_previous_ && !synth_available_) {
    Blink(6, true);  // no wavetables: see README.md
  }
  synth_zone_previous_ = synth_zone;
  seed_mode_ = Zone(s[KNOB_D].value, SEED_MODE_LAST, seed_mode_);

  // Extras (Pearls additions).
  const KnobState* x = knobs_[PAGE_EXTRAS];
  c.reverse = x[KNOB_A].value;
  c.spread = x[KNOB_B].value;
  const int scale = Zone(x[KNOB_C].value, SCALE_LAST, last_scale_zone_);
  if (scale != last_scale_zone_) {
    last_scale_zone_ = scale;
    Blink(scale + 1, false);
  }
  c.scale = scale;
  // Sample slots: 0 = none, then 1..8. Choosing a slot loads it.
  const int sample = Zone(x[KNOB_D].value, kNumSampleSlots + 1,
                          last_sample_zone_);
  if (sample != last_sample_zone_) {
    last_sample_zone_ = sample;
    sample_request_ = sample;
    if (!sample) {
      Blink(1, false);
    }
  }

  c.freeze = freeze_latched_;
  // The SEED press, without the hold of a page change gesture (in button
  // mode, the hold leading up to it is a SEED press, as on Beads).
  const bool seed_press =
      (button_[BUTTON_SEED] && !seed_combo_) || seed_pulse_;
  c.seed_button = seed_mode_ == SEED_MODE_BUTTON && seed_press;
  c.tap = seed_mode_ == SEED_MODE_TAP && seed_press;
}

void LichPearls::UpdateLeds(Outputs* outputs, size_t size) {
  const Status& status = engine_->status();
  const size_t blink_on = size_t(kBlinkOn * sample_rate_);
  const size_t blink_period = size_t(kBlinkPeriod * sample_rate_);
  const size_t t = time_ - blink_start_;

  bool led_freeze = status.frozen;
  bool led_seed;
  if (status.seed_led) {
    grain_led_time_ = size_t(kGrainLedTime * sample_rate_);
  }
  if (grain_led_time_) {
    grain_led_time_ = grain_led_time_ > size ? grain_led_time_ - size : 0;
    led_seed = true;
  } else {
    // Dim while locked and idle, as the Beads SEED LED.
    led_seed = status.seed_locked && (time_ / size) % kDimDivider == 0;
  }

  if (page_ != PAGE_GRAINS) {
    // Beacon: the page number in short pulses every 2 s on the SEED LED.
    const size_t repeat = size_t(kBeaconRepeat * sample_rate_);
    const size_t period = size_t(kBeaconPeriod * sample_rate_);
    const size_t on = size_t(kBeaconOn * sample_rate_);
    const size_t phase = time_ % repeat;
    led_seed = phase < size_t(page_ + 1) * period && (phase % period) < on;
  }

  if (sample_reading_ || sample_busy_) {
    // Loading a sample: the FREEZE LED flickers.
    led_freeze = (time_ / size_t(0.05f * sample_rate_)) & 1;
  }
  const size_t period = fast_blink_ ? blink_period / 3 : blink_period;
  const size_t on = fast_blink_ ? blink_on / 3 : blink_on;
  if (blink_count_ && t < size_t(blink_count_) * period) {
    // Page or setting change: both LEDs blink the number. Errors: fast.
    led_freeze = led_seed = (t % period) < on;
  } else {
    blink_count_ = 0;
  }
  outputs->led[BUTTON_FREEZE] = led_freeze;
  outputs->led[BUTTON_SEED] = led_seed;
  ui_.page = page_;
  ui_.frozen = status.frozen;
  ui_.seed_locked = status.seed_locked;
}

void LichPearls::Process(
    const Inputs& inputs, const float* in_l, const float* in_r,
    float* out_l, float* out_r, size_t size, Outputs* outputs) {
  size = std::min(size, kMaxBlockSize);
  ReadKnobs(inputs);
  // A short trigger within the block: a press at the start of the block,
  // released at its end.
  seed_pulse_ = false;
  if ((inputs.pulse[BUTTON_FREEZE] && !inputs.button[BUTTON_FREEZE] &&
       !button_[BUTTON_FREEZE]) ||
      (inputs.pulse[BUTTON_SEED] && !inputs.button[BUTTON_SEED] &&
       !button_[BUTTON_SEED])) {
    Inputs pressed = inputs;
    for (int b = 0; b < NUM_BUTTONS; ++b) {
      if (inputs.pulse[b] && !inputs.button[b] && !button_[b]) {
        pressed.button[b] = true;
        pressed.edge_frame[b] = -1;
      }
    }
    seed_pulse_ = pressed.button[BUTTON_SEED] && !inputs.button[BUTTON_SEED];
    UpdateButtons(pressed, 0);
  }
  UpdateButtons(inputs, size);
  if (seed_pulse_) {
    std::fill(&seed_gate_[0], &seed_gate_[std::min(size, size_t(8))], 1);
  }
  UpdateControls();
  UpdateGovernor(size);

  // A finished sample load: frozen, as on the Nebulae.
  if (sample_busy_ && engine_->transfer_state() == TRANSFER_DONE) {
    engine_->EndTransfer();
    sample_busy_ = false;
    Blink(last_sample_zone_ > 0 ? last_sample_zone_ : 1, false);
  }

  uint8_t gate[kMaxBlockSize];
  engine_->Process(*controls_, in_l, in_r, NULL,
                   seed_mode_ == SEED_MODE_CLOCK ? seed_gate_ : NULL,
                   out_l, out_r, gate, size);

  // Gate output: the grain gate. CV 1: a new random value on each grain;
  // CV 2: the output level.
  bool any_gate = false;
  for (size_t i = 0; i < size; ++i) {
    const bool g = gate[i] != 0;
    if (g && !gate_previous_) {
      rng_ = rng_ * 1664525u + 1013904223u;
      random_cv_ = float(rng_ >> 8) / 16777216.0f;
    }
    gate_previous_ = g;
    any_gate = any_gate || g;
    const float level = std::max(fabsf(out_l[i]), fabsf(out_r[i]));
    const float coefficient = level > envelope_ ? 0.01f : 0.0002f;
    envelope_ += coefficient * (level - envelope_);
  }
  outputs->gate = any_gate;
  outputs->cv[0] = random_cv_;
  outputs->cv[1] = std::min(1.0f, 1.5f * envelope_);

  UpdateLeds(outputs, size);
  time_ += size;
}

}  // namespace lich
}  // namespace pearls
