// The sources of the Lich build (included by pearls_dsp.cpp and by the M4
// benchmark).
#include <cstdio>

#include "beads/dsp/processor.cc"
#include "beads/dsp/synth.cc"
#include "beads/dsp/mu_law.cc"
#include "beads/dsp/delay_clock.cc"
#include "beads/dsp/delay_controller.cc"
#include "beads/dsp/trigger_generator.cc"
#include "beads/resources.cc"
#include "stmlib/utils/random.cc"
#include "stmlib/dsp/units.cc"
#include "engine/gain_control.cc"
#include "engine/pearls_engine.cc"
#include "lich/lich_pearls.cc"
