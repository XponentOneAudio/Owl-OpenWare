// Pearls for the Lich: build options of the DSP, shared by the patch and
// the M4 benchmark (tools/m4sim).
//
// - The codec stays at 48 kHz (scorched cassette resamples, as on Nebulae).
// - A shorter 32 kHz resampler, designed in float (CPU, size).
// - The reverb runs at 24 kHz: half the CPU, and its memory then fits in
//   the internal RAM instead of the SDRAM (the biggest saving).
// - No dummy grain players (Beads uses them to waste CPU cycles).
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

#endif  // PEARLS_OWL_LICH_CONFIG_H_
