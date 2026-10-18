# Pearls on the Befaco Lich (OWL2).
#
#   make test      desktop tests: the Lich front end, and the Lich DSP changes
#                  against upstream Beads (tools/check_block_rendering.sh)
#   make patch     the OWL patch (see owl/Makefile for OWLPROGRAM, TOOLROOT)
#   make syx       SysEx files in build/: pearls-lich.syx (load and run),
#                  pearls-lich-slot$(SLOT).syx (store in slot SLOT, default 1),
#                  pearls-waves.syx (synth mode wavetables, store once)
#   make prebuilt  the same in prebuilt/, with the source commit
#   make resource  the wavetable resource for synth mode
#   make m4        Cortex-M4 CPU estimates (see tools/m4sim/Makefile)

CXX ?= g++
BUILD = build
# The Lich's DSP options (owl/lich_config.h), with stmlib's desktop code.
DESKTOP_FLAGS = -O2 -g -Wall -Wno-unused-local-typedefs -DTEST -include cstdio \
	-DBEADS_NO_HARDWARE_24K -DPEARLS_RESAMPLER_TAPS=96 \
	-DPEARLS_FILTER_DESIGN_REAL=float -DBEADS_REVERB_DIVIDER=2 -DBEADS_NO_WASTE_CPU -DPEARLS_TRANSFER_CHUNK=256 -DPEARLS_BLOCK_SIZE=16 \
	-I. -Ithird_party/eurorack
DSP_SRC = third_party/eurorack/beads/dsp/processor.cc third_party/eurorack/beads/dsp/synth.cc \
	third_party/eurorack/beads/dsp/mu_law.cc third_party/eurorack/beads/dsp/delay_clock.cc \
	third_party/eurorack/beads/dsp/delay_controller.cc \
	third_party/eurorack/beads/dsp/trigger_generator.cc third_party/eurorack/beads/resources.cc \
	third_party/eurorack/stmlib/utils/random.cc third_party/eurorack/stmlib/dsp/units.cc \
	engine/pearls_engine.cc engine/gain_control.cc lich/lich_pearls.cc lich/wav_reader.cc

.PHONY: test lich_test check patch sysex size resource m4 syx prebuilt hosttest linear-syx
SLOT ?= 1
test: lich_test check

$(BUILD)/lich_test: $(DSP_SRC) tools/lich_test.cc lich/lich_pearls.h engine/*.h
	mkdir -p $(BUILD)
	$(CXX) $(DESKTOP_FLAGS) -o $@ $(DSP_SRC) tools/lich_test.cc

# Also as on the Lich: wavetables loaded from the resource.
$(BUILD)/lich_test_ext: $(DSP_SRC) lich/wav_reader.cc tools/lich_test.cc lich/*.h engine/*.h
	mkdir -p $(BUILD)
	$(CXX) $(DESKTOP_FLAGS) -DBEADS_EXTERNAL_WAVETABLES -o $@ $(DSP_SRC) tools/lich_test.cc

# The OWL patch class against a stand-in for the OwlProgram API.
$(BUILD)/patch_test: $(DSP_SRC) tools/patch_test.cc tools/owl_mock/Patch.h owl/PearlsPatch.hpp lich/*.h engine/*.h
	mkdir -p $(BUILD)
	$(CXX) $(DESKTOP_FLAGS) -DBEADS_EXTERNAL_WAVETABLES -Itools/owl_mock -o $@ $(DSP_SRC) tools/patch_test.cc

lich_test: $(BUILD)/lich_test $(BUILD)/lich_test_ext $(BUILD)/patch_test resource
	$(BUILD)/lich_test
	$(BUILD)/lich_test_ext $(BUILD)/pearls-waves.res
	$(BUILD)/patch_test $(BUILD)/pearls-waves.res
	$(BUILD)/patch_test

check:
	./tools/check_block_rendering.sh

OWLPROGRAM ?= ../../../OwlProgram
TOOLROOT ?=
export OWLPROGRAM TOOLROOT

patch sysex size:
	$(MAKE) -C owl $@

resource:
	python3 tools/make_wavetable_resource.py

m4:
	$(MAKE) -C tools/m4sim run

# Runs the patch binary itself under QEMU, with a stand-in for the firmware.
hosttest: patch
	TOOLROOT=$(TOOLROOT) tools/owlhost/run.sh $(abspath $(BUILD))/owl/patch.bin

syx: patch resource $(BUILD)/check_sysex
	python3 tools/make_sysex.py $(BUILD)/owl/patch.bin $(BUILD)/pearls-lich.syx
	python3 tools/make_sysex.py $(BUILD)/owl/patch.bin \
		$(BUILD)/pearls-lich-slot$(SLOT).syx --store $(SLOT)
	python3 tools/make_sysex.py $(BUILD)/pearls-waves.res \
		$(BUILD)/pearls-waves.syx --save pearls-waves --strip-resource-header
	$(BUILD)/check_sysex $(BUILD)/pearls-lich.syx $(BUILD)/owl/patch.bin
	$(BUILD)/check_sysex $(BUILD)/pearls-lich-slot$(SLOT).syx $(BUILD)/owl/patch.bin

# Decodes .syx files with OpenWare's own sysex.c and crc32.c.
OPENWARE_SOURCE ?= ../../Source
$(BUILD)/check_sysex: tools/check_sysex.c
	mkdir -p $(BUILD)
	$(CC) -O2 -I$(OPENWARE_SOURCE) -o $@ tools/check_sysex.c \
		$(OPENWARE_SOURCE)/sysex.c $(OPENWARE_SOURCE)/crc32.c

# The variant with linear interpolation for the delay taps (owl/Makefile
# LINEAR_TAPS), built before the default one.
linear-syx:
	$(MAKE) -C owl patch LINEAR_TAPS=1
	python3 tools/make_sysex.py $(BUILD)/owl/patch.bin $(BUILD)/pearls-lich-linear.syx

prebuilt: linear-syx syx
	mkdir -p prebuilt
	cp $(BUILD)/owl/patch.bin prebuilt/pearls-lich.bin
	cp $(BUILD)/pearls-lich.syx $(BUILD)/pearls-lich-slot$(SLOT).syx \
		$(BUILD)/pearls-waves.syx $(BUILD)/pearls-lich-linear.syx prebuilt/
	( echo "Source: $$(git rev-parse --short HEAD)$$(git diff --quiet HEAD -- . ':!prebuilt' || echo ' (modified)')"; \
	  echo "Toolchain: $$($(TOOLROOT)arm-none-eabi-gcc --version | head -1)"; \
	  echo "OwlProgram: $$(git -C $(OWLPROGRAM) rev-parse --short HEAD 2>/dev/null)"; \
	  echo "Patch size: $$(stat -c %s $(BUILD)/owl/patch.bin) bytes (limit 147456)" ) > prebuilt/BUILD_INFO.txt
	cat prebuilt/BUILD_INFO.txt
