// Pearls for the Lich: build options of the DSP, shared by the patch and
// the M4 benchmark (tools/m4sim).
//
// - The codec stays at 48 kHz (scorched cassette resamples, as on Nebulae).
// - A shorter 32 kHz resampler, designed in float (CPU, size).
// - stmlib's embedded code paths (inline vsqrt and ssat), but no .ramtext
//   section: OWL patches run from RAM anyway.

#ifndef PEARLS_OWL_LICH_CONFIG_H_
#define PEARLS_OWL_LICH_CONFIG_H_

#define BEADS_NO_HARDWARE_24K
#define PEARLS_RESAMPLER_TAPS 96
#define PEARLS_FILTER_DESIGN_REAL float

#include "stmlib/stmlib.h"
#undef IN_RAM
#define IN_RAM

#endif  // PEARLS_OWL_LICH_CONFIG_H_
