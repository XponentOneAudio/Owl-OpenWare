# Pearls on the Befaco Lich (OWL2).
#
#   make test      desktop tests: the Lich front end, and the Lich DSP changes
#                  against upstream Beads (tools/check_block_rendering.sh)
#   make patch     the OWL patch (see owl/Makefile for OWLPROGRAM, TOOLROOT)
#   make sysex     the patch as a .syx file
#   make resource  the wavetable resource for synth mode
#   make m4        Cortex-M4 CPU estimates (see tools/m4sim/Makefile)

CXX ?= g++
BUILD = build
# The Lich's DSP options (owl/lich_config.h), with stmlib's desktop code.
DESKTOP_FLAGS = -O2 -g -Wall -Wno-unused-local-typedefs -DTEST -include cstdio \
	-DBEADS_NO_HARDWARE_24K -DPEARLS_RESAMPLER_TAPS=96 \
	-DPEARLS_FILTER_DESIGN_REAL=float -I. -Ithird_party/eurorack
DSP_SRC = third_party/eurorack/beads/dsp/processor.cc third_party/eurorack/beads/dsp/synth.cc \
	third_party/eurorack/beads/dsp/mu_law.cc third_party/eurorack/beads/dsp/delay_clock.cc \
	third_party/eurorack/beads/dsp/delay_controller.cc \
	third_party/eurorack/beads/dsp/trigger_generator.cc third_party/eurorack/beads/resources.cc \
	third_party/eurorack/stmlib/utils/random.cc third_party/eurorack/stmlib/dsp/units.cc \
	engine/pearls_engine.cc engine/gain_control.cc lich/lich_pearls.cc

.PHONY: test lich_test check patch sysex size resource m4
test: lich_test check

$(BUILD)/lich_test: $(DSP_SRC) tools/lich_test.cc lich/lich_pearls.h engine/*.h
	mkdir -p $(BUILD)
	$(CXX) $(DESKTOP_FLAGS) -o $@ $(DSP_SRC) tools/lich_test.cc

lich_test: $(BUILD)/lich_test
	$(BUILD)/lich_test

check:
	./tools/check_block_rendering.sh

patch sysex size:
	$(MAKE) -C owl $@

resource:
	python3 tools/make_wavetable_resource.py

m4:
	$(MAKE) -C tools/m4sim run
