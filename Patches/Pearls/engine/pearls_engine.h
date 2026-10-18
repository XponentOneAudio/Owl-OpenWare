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
// Pearls: the Beads granular processor as a host-independent engine.
//
// This class replaces the hardware glue of beads/beads.cc (codec callback,
// normalisation detection, UI state machine) with logic that works from plain
// control values and per-sample gate arrays. Hosts (Csound opcode, desktop
// renderer, tests) feed it 48 kHz stereo audio in blocks of any size.

#ifndef PEARLS_ENGINE_PEARLS_ENGINE_H_
#define PEARLS_ENGINE_PEARLS_ENGINE_H_

#include <cstddef>
#include <cstdint>

#include "beads/dsp/processor.h"
#include "beads/dsp/synth.h"

#include "engine/gain_control.h"
#include "engine/rational_resampler.h"

namespace pearls {

// Lich port: when offset is not 0, the hi-fi processor runs through the
// alias of its code at its address minus offset (see pearls_engine.cc).
void SetFastCodeOffset(uintptr_t offset);

// Beads "media" settings. The Beads manual's names are in quotes; its
// quality button steps downwards (3, 2, 1, 0, 3...).
enum Quality {
  QUALITY_DIRTY_TAPE,       // 0 "Scorched cassette": 8-bit mu-law, wow & flutter
  QUALITY_CLEAN_TAPE,       // 1 "Sunny tape": 12-bit at 24 kHz
  QUALITY_VINTAGE_DIGITAL,  // 2 "Cold digital": 12-bit at 32 kHz (Clouds)
  QUALITY_DIGITAL,          // 3 Hi-fi: 16-bit at 48 kHz
  QUALITY_LAST
};

// Pitch quantisation scales (Pearls addition): grain pitches snap to the
// scale's notes, the root being the unison (the recorded pitch).
enum Scale {
  SCALE_OFF,
  SCALE_CHROMATIC,
  SCALE_MAJOR,
  SCALE_MINOR,
  SCALE_MAJOR_PENTATONIC,
  SCALE_MINOR_PENTATONIC,
  SCALE_FIFTHS,             // octaves and fifths
  SCALE_OCTAVES,
  SCALE_LAST
};

// Semitone mask of a scale (bit n = n semitones above the root).
uint16_t ScaleMask(int scale);

// All values are what the Beads firmware computes from its pots and CVs,
// before they reach beads::Processor. See nebulae_mapping.h for the
// conversion from Nebulae control values.
struct Controls {
  float time;          // 0..1
  float size;          // 0..1 (> 0.99 = delay mode)
  float shape;         // 0..1
  float density;       // 0..1 (0.5 = no grains)
  float pitch;         // semitones

  // Internal randomisation ("attenuverter" with no CV patched), -1..+1.
  // > 0: uniform random variation per grain. < 0: sparse random jumps.
  float time_random;
  float size_random;
  float shape_random;
  float pitch_random;

  float feedback;      // 0..1
  float dry_wet;       // 0..1
  float reverb;        // 0..1

  // Pearls additions (0 = Beads behaviour).
  float reverse;       // 0..1, probability of reversed grains
  float spread;        // 0..1, extra random stereo spread of the grains
  int scale;           // Scale: pitch quantisation

  int quality;         // Quality
  bool mono;           // Mono buffer (twice as long), left input only.
  bool synth;          // Wavetable synth mode instead of audio processing.
  bool freeze;         // Latched freeze button.
  bool seed_button;    // Momentary seed button (tap/hold, long press locks).
  bool tap;            // Momentary tap tempo button (taps start a grain
                       // clock, a 1 s press stops it).
  uint8_t gain;        // 0 = automatic gain control, 1..255 = manual.

  void SetDefaults();
};

struct Status {
  bool seed_locked;
  bool seed_led;         // grain/tap activity, as the Beads seed LED
  bool frozen;
  bool input_present;
  bool seed_input_patched;
  float overwrite_reserve;
  uint8_t vu_meter;

  bool latched_frozen;   // frozen by the button (or a load), not the gate
  bool gain_adjusting;   // automatic gain control measuring the input
  bool clock_running;    // tap tempo clock
  bool clock_led;        // tap tempo clock beat
};

// A buffer load or save (see Engine::StartLoad and Engine::StartSave).
enum TransferState {
  TRANSFER_IDLE,
  TRANSFER_LOADING,
  TRANSFER_SAVING,
  TRANSFER_DONE
};

class Engine {
 public:
  static const int kSampleRate = 48000;
  // Beads processes 12 frames at a time at 48 kHz (4 kHz block rate). The
  // Lich uses 16, which divides its 32-frame audio blocks: each then does
  // the same amount of work (with 12, they alternate between 3 and 2
  // blocks, 12% more at peak).
#ifndef PEARLS_BLOCK_SIZE
#define PEARLS_BLOCK_SIZE 12
#endif
  static const size_t kBlockSize = PEARLS_BLOCK_SIZE;
  // Cold digital at 32 kHz: with a block size that isn't a multiple of 3,
  // the resamplers stream (blocks of 10 or 12 frames at 32 kHz), with a
  // few frames of extra latency.
  static const bool kStreaming32k = kBlockSize % 3 != 0;
  static const size_t kMax32kBlock = (2 * kBlockSize + 2) / 3 + 1;
  static const size_t kStreamingLatency = 4;
  // Latency added by the internal block FIFO, in frames.
  static const size_t kLatency = kBlockSize;

  Engine()
      : max_grains_(32),
        delay_buffer_(NULL),
        reverb_buffer_(NULL),
        processor_(NULL),
        synth_(NULL) { }
  ~Engine();

  // Allocates the delay buffers. Call once, not from an audio thread.
  bool Init();

  // Renders `size` frames. Gate arrays hold one byte per frame (non-zero =
  // gate high) and may be NULL when the corresponding jack is not available.
  // gate_out may be NULL.
  void Process(
      const Controls& controls,
      const float* in_l,
      const float* in_r,
      const uint8_t* freeze_gate,
      const uint8_t* seed_gate,
      float* out_l,
      float* out_r,
      uint8_t* gate_out,
      size_t size);

  const Status& status() const { return status_; }

  // Tests: blocks the streaming 32 kHz path couldn't fill (should be 0).
  size_t streaming_underflows() const { return streaming_underflows_; }

  // Locks or unlocks the grain generator directly (Beads: a 4 s SEED press,
  // or SEED + FREEZE), for hosts whose SEED button is also the seed input.
  void set_seed_locked(bool locked) { seed_locked_ = locked; }

  // Grain voices, 4..32 (Beads: 32). Lowered by hosts with less CPU than
  // Beads' (the Lich patch adjusts it from its measured CPU load).
  void set_max_grains(int max_grains) { max_grains_ = max_grains; }
  int max_grains() const { return max_grains_; }

  // Restarts the automatic gain control's measurement of the input level
  // (Beads: long press on the quality button).
  void RestartGainControl();

  // Buffer transfers. They run over several Process() calls, in chunks small
  // enough for an audio thread; the arrays must stay valid until
  // transfer_state() is TRANSFER_DONE (then call EndTransfer()).
  //
  // StartLoad fills the recording buffer with `frames` frames at
  // `sample_rate`, converted to the rate of the current quality setting and
  // truncated to the buffer length. The output is muted meanwhile. With
  // `hold`, the buffer is then frozen as if the FREEZE button had been
  // pressed: the next press releases it.
  bool StartLoad(const float* l, const float* r, size_t frames,
                 float sample_rate, bool hold);
  // The same, reading frames through a callback (the Lich reads them from a
  // WAV file in memory, without a float copy). context must stay valid
  // until the transfer is done.
  typedef void (*FrameReader)(const void* context, size_t frame,
                              float* l, float* r);
  bool StartLoad(FrameReader reader, const void* context, size_t frames,
                 float sample_rate, bool hold);
  // StartSave copies the buffer contents, oldest frame first, into arrays of
  // at least `capacity` frames.
  bool StartSave(float* l, float* r, size_t capacity);
  TransferState transfer_state() const { return transfer_state_; }
  void EndTransfer() { transfer_state_ = TRANSFER_IDLE; }
  // Result of the last save.
  size_t saved_frames() const { return transfer_frames_; }
  float saved_sample_rate() const { return transfer_rate_; }
  bool saved_mono() const { return transfer_mono_; }
  // Buffer length (in frames) for any quality setting: the size of arrays
  // that can hold every save.
  static size_t MaxBufferFrames();

  // Incremented whenever the buffer contents change (recording, loading,
  // reconfiguration): tells hosts whether a saved copy is still current.
  uint32_t buffer_generation() const { return buffer_generation_; }

 private:
  void RenderBlock();
  void UpdatePitch();
  void UpdateTapClock(bool* pulses);
  // Sample loading and saving: once per block, kept small.
  __attribute__((noinline, optimize("Os"))) void ContinueTransfer();
  void ResetStreaming();
  void ProcessAudio32kStreaming(
      beads::SeedButtonState seed_state,
      const stmlib::GateFlags* seed,
      const bool* freeze,
      const beads::ModulatedParameter& d,
      const beads::Parameters& p,
      const beads::Processor::MixParameters& m,
      beads::FloatFrame* in,
      beads::FloatFrame* out,
      bool* gate);
  static void ReadLoadArrays(const void* context, size_t frame,
                             float* l, float* r);
  void UpdateSeedButton(bool pressed, bool freeze_toggled);
  void UpdateConfiguration();
  void ApplyConfiguration();
  void DetectInput(const beads::FloatFrame* in, size_t size);
  void BuildParameters(beads::Parameters* p, beads::ModulatedParameter* d,
                       beads::Processor::MixParameters* m) const;
  void ProcessAudio(
      beads::SeedButtonState seed_state,
      const stmlib::GateFlags* seed,
      const bool* freeze,
      const beads::ModulatedParameter& d,
      const beads::Parameters& p,
      const beads::Processor::MixParameters& m,
      beads::FloatFrame* in,
      beads::FloatFrame* out,
      bool* gate);

  int max_grains_;

  // Large buffers, allocated in Init().
  uint8_t* delay_buffer_;
  uint16_t* reverb_buffer_;

  beads::Processor* processor_;
  beads::Synth* synth_;
  GainControl gain_control_;

  // 48 kHz <-> 32 kHz for the vintage digital mode.
  // Filter length (192 on the Nebulae; the Lich uses 96 to save CPU, which
  // widens the transition band above the 14.5 kHz cutoff).
#ifndef PEARLS_RESAMPLER_TAPS
#define PEARLS_RESAMPLER_TAPS 192
#endif
  RationalResampler<2, 3, PEARLS_RESAMPLER_TAPS, kBlockSize> down_;
  RationalResampler<3, 2, PEARLS_RESAMPLER_TAPS, kMax32kBlock> up_;

  // Streaming 32 kHz path: input frames waiting for an even block (the
  // 24 kHz reverb pairs samples), and the 48 kHz output FIFO.
  size_t pending32_;
  beads::FloatFrame pending_in32_[kMax32kBlock];
  bool pending_freeze32_[kMax32kBlock];
  stmlib::GateFlags pending_seed32_[kMax32kBlock];
  static const size_t kOutFifoSize = 64;
  size_t out48_count_;
  beads::FloatFrame out48_[kOutFifoSize];
  bool out48_gate_[kOutFifoSize];
  size_t streaming_underflows_;

  Controls controls_;
  Status status_;

  // Block FIFO.
  size_t fifo_index_;
  float in_fifo_l_[kBlockSize];
  float in_fifo_r_[kBlockSize];
  bool freeze_fifo_[kBlockSize];
  bool seed_fifo_[kBlockSize];
  float out_fifo_l_[kBlockSize];
  float out_fifo_r_[kBlockSize];
  bool gate_fifo_[kBlockSize];

  // Configuration state (quality, mono, synth) with a short fade on change.
  bool synth_mode_;
  int target_quality_;
  bool target_mono_;
  bool target_synth_;
  float fade_;
  bool reconfigure_pending_;
  bool reset_gain_pending_;

  // Seed button state machine (as beads/ui.cc).
  bool seed_locked_;
  bool seed_pressed_;
  bool seed_combo_;
  size_t seed_press_duration_;

  // Seed gate: patch detection by activity, 2.5 ms delay, gate flags.
  bool seed_gate_previous_;
  size_t seed_idle_time_;
  bool seed_delay_line_[128];
  size_t seed_delay_write_;
  stmlib::GateFlags seed_flags_previous_;
  stmlib::GateFlags seed_flags_previous_32k_;

  // Input presence detection, replacing the Beads normalisation probe.
  float input_envelope_;
  size_t input_silence_time_;

  // Pitch after quantisation, with hysteresis.
  float pitch_;

  // Grain gate LED stretcher.
  size_t seed_led_time_;

  // FREEZE button state, inverted by loads and the SEED + FREEZE combo.
  bool freeze_inverted_;
  bool freeze_button_previous_;
  // Configuration changes wait while frozen.
  bool frozen_previous_block_;
  bool configured_;
  bool restart_gain_pending_;

  // Tap tempo clock.
  bool tap_previous_;
  size_t tap_press_duration_;
  size_t since_last_tap_;
  float tap_intervals_[4];
  size_t num_tap_intervals_;
  bool clock_running_;
  float clock_period_;
  float clock_phase_;
  size_t clock_pulse_remaining_;
  size_t clock_led_time_;

  // Buffer transfer.
  TransferState transfer_state_;
  const float* load_l_;
  const float* load_r_;
  FrameReader load_reader_;
  const void* load_context_;
  float* save_l_;
  float* save_r_;
  size_t transfer_source_frames_;
  float transfer_source_rate_;
  bool transfer_hold_;
  bool transfer_started_;
  double transfer_position_;
  size_t transfer_frames_;
  size_t transfer_target_frames_;
  float transfer_rate_;
  bool transfer_mono_;
  int transfer_write_ptr_;
  uint32_t buffer_generation_;

  DISALLOW_COPY_AND_ASSIGN(Engine);
};

}  // namespace pearls

#endif  // PEARLS_ENGINE_PEARLS_ENGINE_H_
