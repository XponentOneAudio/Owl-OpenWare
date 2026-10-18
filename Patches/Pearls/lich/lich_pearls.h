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
// Pearls on the Befaco Lich: the panel (paged knobs, button gestures, LEDs,
// outputs) and the CPU governor around the Pearls engine.
//
// This header is host-independent and includes nothing from the engine or
// from OWL: the OWL patch includes it after Patch.h, whose basicmaths.h
// redefines sin(), sqrt() and others as macros.
//
// The Lich has four knobs (each summed with its 0-10 V CV input), two buttons
// (each OR'd with a gate input by the firmware, so a press and a gate cannot
// be told apart), a gate output, two CV outputs and two single-colour LEDs.
// See README.md for the control scheme.

#ifndef PEARLS_LICH_LICH_PEARLS_H_
#define PEARLS_LICH_LICH_PEARLS_H_

#include <cstddef>
#include <cstdint>

#include "lich/wav_reader.h"

namespace pearls {

class Engine;
struct Controls;

// See pearls_engine.h.
void SetFastCodeOffset(uintptr_t offset);

namespace lich {

enum Page {
  PAGE_GRAINS,    // TIME, SIZE, DENSITY, PITCH
  PAGE_MIX,       // SHAPE, FEEDBACK, DRY/WET, REVERB
  PAGE_RANDOM,    // TIME, SIZE, SHAPE, PITCH randomness
  PAGE_EXTRAS,    // REVERSE, SPREAD, SCALE, SAMPLE (Pearls additions)
  PAGE_SETUP,     // QUALITY, INPUT GAIN, STEREO/MONO/SYNTH, SEED MODE
  PAGE_LAST
};

// SEED modes (setup page, knob D).
enum SeedMode {
  SEED_MODE_BUTTON,   // Beads' SEED button
  SEED_MODE_CLOCK,    // Beads' SEED input
  SEED_MODE_TAP,      // tap tempo (Pearls addition)
  SEED_MODE_LAST
};

// Sample slots: resources "pearls-1.wav" to "pearls-8.wav".
const int kNumSampleSlots = 8;

enum Knob { KNOB_A, KNOB_B, KNOB_C, KNOB_D, NUM_KNOBS };
enum Button { BUTTON_FREEZE, BUTTON_SEED, NUM_BUTTONS };

// The panel, read once per audio block.
struct Inputs {
  float knob[NUM_KNOBS];        // 0..1, knob + CV (10 V = full scale)
  bool button[NUM_BUTTONS];     // button OR gate, at the end of the block
  // Frame of the block at which each button last changed, or -1. Lets a
  // trigger land on the right frame instead of the start of the block.
  int edge_frame[NUM_BUTTONS];
  // The button went high and low again within the block (a short trigger):
  // the OWL firmware only reports the level at the end of the block.
  bool pulse[NUM_BUTTONS];
};

struct Outputs {
  bool led[NUM_BUTTONS];        // LED of button 1 (FREEZE) and 2 (SEED)
  bool gate;                    // gate output, level at the end of the block
  float cv[2];                  // CV outputs, 0..1 (0..10 V)
};

// What the front end shows on the LEDs, for tests.
struct UiState {
  int page;
  bool frozen;
  bool seed_locked;
  int max_grains;
  float load;                   // smoothed CPU load seen by the governor
  bool knob_caught[NUM_KNOBS];  // knob controls its parameter on this page
};

class LichPearls {
 public:
  // The largest block Process() accepts.
  static const size_t kMaxBlockSize = 512;

  LichPearls();
  ~LichPearls();

  // Allocates the engine (about 900 kB, mostly in SDRAM on the Lich).
  bool Init(float sample_rate);

  // Renders one block. out_l/out_r may be in_l/in_r.
  void Process(const Inputs& inputs, const float* in_l, const float* in_r,
               float* out_l, float* out_r, size_t size, Outputs* outputs);

  // CPU governor: the host reports the share of the block period the last
  // Process() call took (0..1+). With no reports, the grain voice limit
  // stays at kDefaultMaxGrains.
  void ReportLoad(float load);

  // The Beads wavetables (beads/resources.cc: wav_integrated_waves), when
  // the build keeps them out of the binary (BEADS_EXTERNAL_WAVETABLES).
  // Returns false (synth mode unavailable) if the data is missing or wrong.
  bool set_wavetables(const void* data, size_t bytes);
  bool synth_available() const { return synth_available_; }

  // Samples. The host polls TakeSampleRequest() for a slot (1..8) chosen
  // on the extras page, reads that resource, and passes the WAV file to
  // LoadSample() (or calls SampleFailed()), keeping it until sample_busy()
  // turns false. SampleReading() makes the LEDs show the read.
  int TakeSampleRequest();
  void SampleReading();
  bool LoadSample(const void* wav, size_t bytes);
  void SampleFailed();
  bool sample_busy() const { return sample_busy_; }

  // Upper limit of the governor (4..32).
  void set_grain_ceiling(int grains);

  const UiState& ui_state() const { return ui_; }

  // Parameter values as the engine sees them (for tests).
  const Controls& controls() const;

  static const int kDefaultMaxGrains = 8;

 private:
  struct KnobState {
    float value;          // parameter value, 0..1 in knob units
    bool caught;          // the knob moves the parameter
    float last_raw;
  };

  void ReadKnobs(const Inputs& inputs);
  void Blink(int count, bool fast);
  void ChangePage(int page);
  void UpdateButtons(const Inputs& inputs, size_t size);
  void UpdateControls();
  void UpdateLeds(Outputs* outputs, size_t size);
  void UpdateGovernor(size_t size);

  Engine* engine_;
  Controls* controls_;
  float sample_rate_;

  int page_;
  KnobState knobs_[PAGE_LAST][NUM_KNOBS];
  float raw_[NUM_KNOBS];

  // Buttons.
  bool button_[NUM_BUTTONS];
  size_t held_time_[NUM_BUTTONS];   // frames
  bool freeze_latched_;
  bool freeze_momentary_;           // press held long: release unfreezes
  bool freeze_combo_;               // FREEZE press used for a page change
  bool seed_combo_;                 // SEED hold used for a page change
  int seed_mode_;                   // SeedMode
  bool seed_pulse_;                 // a SEED trigger within this block
  uint8_t seed_gate_[kMaxBlockSize];

  // LEDs.
  size_t time_;                     // frames since Init
  size_t blink_start_;              // page / setting change blink
  int blink_count_;
  size_t grain_led_time_;
  int last_quality_zone_;
  int last_scale_zone_;
  int last_sample_zone_;
  bool fast_blink_;                 // error blink
  bool synth_zone_previous_;
  bool sample_reading_;

  // Samples.
  int sample_request_;
  bool sample_busy_;
  WavInfo wav_info_;

  // Outputs.
  float random_cv_;
  float envelope_;
  bool gate_previous_;
  uint32_t rng_;

  // Governor.
  float load_;
  float load_peak_;
  size_t since_change_;
  int max_grains_;
  int grain_ceiling_;
  bool load_reported_;

  bool synth_available_;

  UiState ui_;
};

}  // namespace lich
}  // namespace pearls

#endif  // PEARLS_LICH_LICH_PEARLS_H_
