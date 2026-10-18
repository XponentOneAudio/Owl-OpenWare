// Pearls for the Lich: build options of the DSP, shared by the patch and
// the M4 benchmark (tools/m4sim).
//
// - The codec stays at 48 kHz (scorched cassette resamples, as on Nebulae).
// - A shorter 32 kHz resampler, designed in float (CPU, size).
// - The reverb runs at 24 kHz: half the CPU, and its memory then fits in
//   the internal RAM instead of the SDRAM (the biggest saving).
// - No dummy grain players (Beads uses them to waste CPU cycles).
// - Sample loads copy 256 frames per block (the M4 has no double FPU).
// - The engine works in blocks of 16 frames, which divide the Lich's audio
//   blocks (32 by default): each block then costs the same.
// - stmlib's embedded code paths (inline vsqrt and ssat), but no .ramtext
//   section: OWL patches run from RAM anyway.
// - Inline fabsf (see below).

#ifndef PEARLS_OWL_LICH_CONFIG_H_
#define PEARLS_OWL_LICH_CONFIG_H_

#define BEADS_NO_HARDWARE_24K
#define PEARLS_RESAMPLER_TAPS 96
#define PEARLS_FILTER_DESIGN_REAL float
#define BEADS_REVERB_DIVIDER 2
#define BEADS_NO_WASTE_CPU
#define PEARLS_TRANSFER_CHUNK 256
// Code run once or rarely (setup, configuration, sample loading), compiled
// for size so that the patch fits in its 144 kB.
#define PEARLS_COLD __attribute__((noinline, optimize("Os")))
// Delay mode: the taps worked out every 4 samples (ramps in between).
#define BEADS_DELAY_CONTROL_RATE 4
// Cycle counts of parts of the engine (engine/pearls_profile.h).
#if defined(__arm__) && !defined(PEARLS_NO_PROFILE)
#define PEARLS_PROFILE
#endif
#ifndef PEARLS_BLOCK_SIZE
#define PEARLS_BLOCK_SIZE 16
#endif

#include "stmlib/stmlib.h"
#undef IN_RAM
#define IN_RAM

// OwlProgram compiles with -fno-builtin, which turns fabsf() into a library
// call (4% of the CPU in delay mode). The builtins still inline: use them,
// after the math headers have declared the functions.
#include <math.h>
#include <cmath>
#define fabsf(x) __builtin_fabsf(x)
#define fabs(x) __builtin_fabs(x)
#define sqrtf(x) __builtin_sqrtf(x)

#endif  // PEARLS_OWL_LICH_CONFIG_H_
