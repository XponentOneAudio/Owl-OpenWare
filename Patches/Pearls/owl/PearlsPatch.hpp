// Copyright 2026 The Pearls (Beads for Nebulae) contributors.
// MIT licence: see lich/lich_pearls.h.
//
// Pearls on the Befaco Lich (OWL2): the OWL patch. The panel logic and the
// engine are in lich/lich_pearls.{h,cc}; this class only moves values
// between OpenWare and them.

#ifndef __PearlsPatch_hpp__
#define __PearlsPatch_hpp__

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
    // The wavetable synth's tables, stored once in the Lich's flash (see
    // README.md). Without them, everything but synth mode works.
    Resource* waves = Resource::load("pearls-waves");
    if (waves && waves->hasData()) {
      pearls_.set_wavetables(waves->getData(), waves->getSize());
    }
    ok_ = pearls_.Init(getSampleRate());
    cycles_per_block_ = 0.0f;
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
    if (!ok_) {
      buffer.clear();
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

    // CPU governor: OpenWare resets the cycle counter before each block.
    if (cycles_per_block_ == 0.0f) {
      cycles_per_block_ = float(size) * 180e6f / getSampleRate();
    }
    pearls_.ReportLoad(float(getElapsedCycles()) / cycles_per_block_);
  }

 private:
  pearls::lich::LichPearls pearls_;
  bool level_[pearls::lich::NUM_BUTTONS];
  bool pulse_[pearls::lich::NUM_BUTTONS];
  int led_[pearls::lich::NUM_BUTTONS];
  int gate_;
  bool ok_;
  float cycles_per_block_;
};

#endif  // __PearlsPatch_hpp__
