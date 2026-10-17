# Cortex-M4 cycle estimates of the Pearls engine, without a Lich.
#
#   make plugin   QEMU plugin (needs glib headers; qemu-plugin.h is fetched)
#   make run      benchmark all configurations (needs qemu-system-arm and
#                 arm-none-eabi-gcc)
#   make run ONLY=11 PROFILE=1   one configuration, with a function profile
#   make run MAX_GRAINS=8        with a grain voice limit
#
# The cycle model (m4cycles.c, m4costs.py) applies Cortex-M4 instruction
# timings and the Lich's SDRAM timings; see the comments there. Expect it to
# be within about +-25% of the real thing.

ROOT = ../..
QEMU_VERSION = v8.2.2
CXX = arm-none-eabi-g++
CC = arm-none-eabi-gcc
ARCH = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -fsingle-precision-constant
DEFS = $(if $(ONLY),-DONLY=$(ONLY)) $(if $(MAX_GRAINS),-DMAX_GRAINS=$(MAX_GRAINS))
FLAGS = $(ARCH) -O3 -ffast-math -DTEST -DBEADS_NO_HARDWARE_24K -include cstdio \
	-I$(ROOT) -I$(ROOT)/third_party/eurorack -ffunction-sections -fdata-sections $(DEFS)
CXXFLAGS = $(FLAGS) -fno-rtti -fno-exceptions -std=gnu++14 -Wno-psabi
BUILD = $(ROOT)/build/m4sim
SRC = $(addprefix $(ROOT)/, \
	third_party/eurorack/beads/dsp/processor.cc third_party/eurorack/beads/dsp/synth.cc \
	third_party/eurorack/beads/dsp/mu_law.cc third_party/eurorack/beads/dsp/delay_clock.cc \
	third_party/eurorack/beads/dsp/delay_controller.cc \
	third_party/eurorack/beads/dsp/trigger_generator.cc third_party/eurorack/beads/resources.cc \
	third_party/eurorack/stmlib/utils/random.cc third_party/eurorack/stmlib/dsp/units.cc \
	engine/pearls_engine.cc engine/gain_control.cc) bench.cc

.PHONY: plugin run bench FORCE
plugin: $(BUILD)/m4cycles.so

$(BUILD)/qemu-plugin.h:
	mkdir -p $(BUILD)
	curl -sSfL -o $@ https://raw.githubusercontent.com/qemu/qemu/$(QEMU_VERSION)/include/qemu/qemu-plugin.h

$(BUILD)/m4cycles.so: m4cycles.c $(BUILD)/qemu-plugin.h
	gcc -O2 -Wall -shared -fPIC -o $@ m4cycles.c -I$(BUILD) $$(pkg-config --cflags glib-2.0)

bench: $(BUILD)/bench.elf
$(BUILD)/bench.elf: FORCE
	mkdir -p $(BUILD)
	$(CC) $(ARCH) -O2 -c startup.c -o $(BUILD)/startup.o
	$(CXX) $(CXXFLAGS) -T mps2.ld -nostartfiles --specs=rdimon.specs -Wl,--gc-sections \
		-o $@ $(BUILD)/startup.o $(SRC) -lm

run: plugin bench
	./run.sh $(BUILD) $(if $(PROFILE),profile=1)
