// Copyright 2026 The Pearls (Beads for Nebulae) contributors.
// MIT licence: see lich/lich_pearls.h.
//
// Cycle counts of parts of the engine, on the Lich (PEARLS_PROFILE, set by
// owl/lich_config.h): the Cortex-M4's cycle counter, which OpenWare resets
// before each block. The patch reports them (send_sysex.py --info).

#ifndef PEARLS_ENGINE_PEARLS_PROFILE_H_
#define PEARLS_ENGINE_PEARLS_PROFILE_H_

#include <stdint.h>

enum PearlsProfileSlot {
  PEARLS_PROFILE_CONTROLLER,  // grain triggers or delay taps
  PEARLS_PROFILE_GRANULAR,    // grains or taps, the delay line
  PEARLS_PROFILE_REVERB,
  PEARLS_PROFILE_PROCESSOR,   // all of beads::Processor::Process
  PEARLS_PROFILE_ENGINE,      // all of pearls::Engine::RenderBlock
  PEARLS_PROFILE_TAPS,        // delay mode's taps (MultitapDelay::Render)
  PEARLS_PROFILE_WRITE,       // the feedback, limiter and delay line write
  PEARLS_PROFILE_NUM_SLOTS
};

#ifdef PEARLS_PROFILE

struct PearlsProfile {
  uint32_t cycles[PEARLS_PROFILE_NUM_SLOTS];
};
extern PearlsProfile pearls_profile;  // in pearls_engine.cc

#define PEARLS_CYCLES() (*reinterpret_cast<volatile uint32_t*>(0xE0001004))
#define PEARLS_PROFILE_START(var) const uint32_t var = PEARLS_CYCLES()
#define PEARLS_PROFILE_ADD(slot, var) \
  (pearls_profile.cycles[slot] += PEARLS_CYCLES() - (var))

#else

#define PEARLS_PROFILE_START(var)
#define PEARLS_PROFILE_ADD(slot, var)

#endif  // PEARLS_PROFILE

#endif  // PEARLS_ENGINE_PEARLS_PROFILE_H_
