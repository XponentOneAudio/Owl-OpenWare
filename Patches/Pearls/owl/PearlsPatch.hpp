// Copyright 2026 The Pearls (Beads for Nebulae) contributors.
// MIT licence: see lich/lich_pearls.h.
//
// Pearls on the Befaco Lich (OWL2): the OWL patch. The panel logic and the
// engine are in lich/lich_pearls.{h,cc}; this class only moves values
// between OpenWare and them.

#ifndef __PearlsPatch_hpp__
#define __PearlsPatch_hpp__

#include <algorithm>
#include <cstring>

#include "Patch.h"
#include "lich/lich_pearls.h"

class PearlsPatch : public Patch {
 public:
  PearlsPatch() {
    // Knob names for the OWL web interface (page 1).
    registerParameter(PARAMETER_A, "Time");
    registerParameter(PARAMETER_B, "Size");
    registerParameter(PARAMETER_C, "Density");
    registerParameter(PARAMETER_D, "Pitch");
    registerParameter(PARAMETER_F, "Random>");
    registerParameter(PARAMETER_G, "Level>");
    for (int b = 0; b < pearls::lich::NUM_BUTTONS; ++b) {
      level_[b] = false;
      pulse_[b] = false;
      led_[b] = -1;
    }
    gate_ = -1;
    sample_resource_ = NULL;
    sample_data_ = NULL;
    sample_size_ = 0;
    sample_read_ = 0;
    // The wavetable synth's tables, stored once in the Lich's flash (see
    // README.md). Without them, everything but synth mode works.
    waves_ = Resource::load("pearls-waves");
    if (waves_ && waves_->hasData()) {
      pearls_.set_wavetables(waves_->getData(), waves_->getSize());
    }
    ok_ = pearls_.Init(getSampleRate());
    cycles_per_block_ = 0.0f;
    overloaded_blocks_ = 0;
    skip_block_ = false;
  }

  ~PearlsPatch() {
    FinishSampleRead();
    FreeSample();
    pearls_.set_wavetables(NULL, 0);
    Resource::destroy(waves_);
  }

  void buttonChanged(PatchButtonId bid, uint16_t value, uint16_t samples) override {
    int b = bid == BUTTON_A ? 0 : bid == BUTTON_B ? 1 : -1;
    if (b < 0) {
      return;
    }
    const bool high = value != 0;
    // The firmware reports each button once per block, with its level at
    // the end of the block: an unchanged level means a short pulse.
    if (high == level_[b]) {
      pulse_[b] = true;
    }
    level_[b] = high;
  }

  void processAudio(AudioBuffer& buffer) override {
    const int size = buffer.getSize();
    float* l = buffer.getSamples(LEFT_CHANNEL);
    float* r = buffer.getSamples(RIGHT_CHANNEL);
    if (!ok_ || skip_block_) {
      // Overload guard (below): a block of silence that costs nothing.
      buffer.clear();
      skip_block_ = false;
      return;
    }
    pearls::lich::Inputs in;
    in.knob[0] = getParameterValue(PARAMETER_A);
    in.knob[1] = getParameterValue(PARAMETER_B);
    in.knob[2] = getParameterValue(PARAMETER_C);
    in.knob[3] = getParameterValue(PARAMETER_D);
    for (int b = 0; b < pearls::lich::NUM_BUTTONS; ++b) {
      in.button[b] = level_[b];
      in.edge_frame[b] = -1;
      in.pulse[b] = pulse_[b];
      pulse_[b] = false;
    }
    pearls::lich::Outputs out;
    pearls_.Process(in, l, r, l, r, size, &out);

    setButton(BUTTON_A, out.led[0] ? 4095 : 0, 0);
    setButton(BUTTON_B, out.led[1] ? 4095 : 0, 0);
    if (int(out.gate) != gate_) {
      gate_ = out.gate;
      setButton(PUSHBUTTON, out.gate ? 4095 : 0, 0);
    }
    setParameterValue(PARAMETER_F, out.cv[0]);
    setParameterValue(PARAMETER_G, out.cv[1]);

    UpdateSampleLoad();

    // CPU governor: OpenWare resets the cycle counter before each block.
    if (cycles_per_block_ == 0.0f) {
      cycles_per_block_ = float(size) * 180e6f / getSampleRate();
    }
    const float load = float(getElapsedCycles()) / cycles_per_block_;
    pearls_.ReportLoad(load);

    // Overload guard: a patch that never finishes its blocks in time
    // starves the firmware's lower priority tasks, its watchdog resets the
    // module, and the firmware then refuses to start the patch (E). After
    // a run of late blocks, skip one: the firmware catches up while the
    // governor sheds grains.
    overloaded_blocks_ = load > 1.0f ? overloaded_blocks_ + 1 : 0;
    if (overloaded_blocks_ >= kOverloadBlocks) {
      overloaded_blocks_ = 0;
      skip_block_ = true;
    }
  }

 private:
  // Samples: resources "pearls-1.wav" to "pearls-8.wav", read from flash a
  // few kB per block (a whole file at once would stall the audio).
  void UpdateSampleLoad() {
    static const size_t kChunk = 4096;
    static const size_t kMaxSize = 6 << 20;  // the SDRAM has about 7 MB free
    if (sample_resource_) {
      const size_t n = std::min(kChunk, sample_size_ - sample_read_);
      if (sample_resource_->read(sample_data_ + sample_read_, n,
                                 sample_read_) != n) {
        FinishSampleRead();
        FreeSample();
        pearls_.SampleFailed();
        return;
      }
      sample_read_ += n;
      if (sample_read_ == sample_size_) {
        FinishSampleRead();
        if (!pearls_.LoadSample(sample_data_, sample_size_)) {
          FreeSample();
        }
      }
    } else if (sample_data_) {
      if (!pearls_.sample_busy()) {
        FreeSample();  // copied into the buffer
      }
    } else {
      const int slot = pearls_.TakeSampleRequest();
      if (slot <= 0) {
        return;
      }
      // A member: the Resource keeps a pointer to its name for read().
      strcpy(sample_name_, "pearls-0.wav");
      sample_name_[7] = char('0' + slot);
      sample_resource_ = Resource::open(sample_name_);
      sample_size_ = sample_resource_ ? sample_resource_->getSize() : 0;
      if (sample_size_ && sample_size_ <= kMaxSize) {
        sample_data_ = new uint8_t[sample_size_];
      }
      if (!sample_data_) {
        FinishSampleRead();
        pearls_.SampleFailed();
        return;
      }
      sample_read_ = 0;
      pearls_.SampleReading();
    }
  }

  void FinishSampleRead() {
    Resource::destroy(sample_resource_);
    sample_resource_ = NULL;
  }

  void FreeSample() {
    delete[] sample_data_;
    sample_data_ = NULL;
  }

  Resource* waves_;
  Resource* sample_resource_;
  char sample_name_[16];
  uint8_t* sample_data_;
  size_t sample_size_;
  size_t sample_read_;

  pearls::lich::LichPearls pearls_;
  bool level_[pearls::lich::NUM_BUTTONS];
  bool pulse_[pearls::lich::NUM_BUTTONS];
  int led_[pearls::lich::NUM_BUTTONS];
  int gate_;
  bool ok_;
  float cycles_per_block_;
  static const int kOverloadBlocks = 16;
  int overloaded_blocks_;
  bool skip_block_;
};

#endif  // __PearlsPatch_hpp__
