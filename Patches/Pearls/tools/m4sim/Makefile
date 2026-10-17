# Cortex-M4 cycle estimates of the Pearls engine, without a Lich.
#
#   make plugin   QEMU plugin (needs glib headers; qemu-plugin.h is fetched)
#   make run      benchmark all configurations (needs qemu-system-arm and
#                 arm-none-eabi-gcc)
#   make run ONLY=11 PROFILE=1   one configuration, with a function profile
#   make run MAX_GRAINS=8        with a grain voice limit
#   TOOLROOT=<dir>/              the toolchain the patch is built with (gcc
#                                10; gcc 13 refuses -ffreestanding C++)
#
# The cycle model (m4cycles.c, m4costs.py) applies Cortex-M4 instruction
# timings and the Lich's SDRAM timings; see the comments there. Expect it to
# be within about +-25% of the real thing.

ROOT = ../..
QEMU_VERSION = v8.2.2
TOOLROOT ?=
CXX = $(TOOLROOT)arm-none-eabi-g++
CC = $(TOOLROOT)arm-none-eabi-gcc
ARCH = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -fsingle-precision-constant
DEFS = $(if $(ONLY),-DONLY=$(ONLY)) $(if $(MAX_GRAINS),-DMAX_GRAINS=$(MAX_GRAINS)) $(if $(REVERB),-DREVERB=$(REVERB))
# The flags OwlProgram compiles patches with (compile.mk), minus -fpic/-fpie.
FLAGS = $(ARCH) -O3 -ffast-math -DNDEBUG -mtune=cortex-m4 -nostdlib -fno-builtin \
	-ffreestanding -mno-unaligned-access -fno-omit-frame-pointer -flto \
	-fdata-sections -ffunction-sections \
	-include lich_config.h -I$(ROOT)/owl -I$(ROOT) -I$(ROOT)/third_party/eurorack $(DEFS)
CXXFLAGS = $(FLAGS) $(if $(DEBUG),-g) -fno-rtti -fno-exceptions -std=gnu++14 -Wno-psabi
BUILD = $(ROOT)/build/m4sim
SRC = bench_dsp.cpp bench.cc

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
		-o $@ $(BUILD)/startup.o $(SRC) -Wl,--start-group -lc -lm -lgcc -lrdimon -Wl,--end-group
	@arm-none-eabi-nm $@ | grep -q wav_integrated_waves || true

run: plugin bench
	./run.sh $(BUILD) $(if $(PROFILE),profile=1)
