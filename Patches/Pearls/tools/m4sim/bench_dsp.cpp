// The Lich build of the DSP for the M4 benchmark: same sources and options
// as owl/pearls_dsp.cpp, but with the wavetables built in (the linker script
// puts them in "SDRAM", as the patch loads them there).
#include "lich_config.h"
#include "pearls_sources.inc"
