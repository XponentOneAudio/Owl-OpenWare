// Pearls for the Lich: the Pearls engine, the Beads DSP and the Lich front
// end in one translation unit, as OwlProgram builds every .cpp of a flat
// patch directory with fixed flags.

#include "lich_config.h"

// The wavetables (100 kB) don't fit in the patch: they are a resource,
// loaded at run time (see PearlsPatch.hpp).
#define BEADS_EXTERNAL_WAVETABLES

#include "pearls_sources.inc"
