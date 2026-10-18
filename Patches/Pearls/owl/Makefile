# Pearls for the Befaco Lich (OWL2): builds the OWL patch with OwlProgram.
#
#   make OWLPROGRAM=<OwlProgram checkout> [TOOLROOT=<arm gcc bin dir>/]
#   make sysex      patch as a .syx file, to load from the OWL web interface
#   make load       load into RAM over USB MIDI (needs FirmwareSender)
#   make store SLOT=n   store in slot n
#   make size       sizes against the 144 kB patch limit
#   make size LDSCRIPT=<script with a larger PATCHRAM>   sizes of a patch
#                   that doesn't fit yet
#
# OwlProgram needs an arm-none-eabi-gcc no newer than 12 (its -ffreestanding
# build refuses the hosted C++ headers of gcc 13); its CI uses 10-2020-q4.
# Run "make libs" in OwlProgram once with the same toolchain.

OWLPROGRAM ?= ../../../../OwlProgram
TOOLROOT ?=
ROOT := $(abspath ..)
STAGE := $(ROOT)/build/owl-src
BUILDDIR := $(ROOT)/build/owl
# OwlProgram's owl2.ld, fixed: see owl2-pearls.ld.
LDSCRIPT ?= $(abspath owl2-pearls.ld)
OWL_ARGS = -C $(OWLPROGRAM) PLATFORM=OWL2 PATCHNAME=Pearls PATCHSOURCE=$(STAGE) \
	BUILD=$(BUILDDIR) TOOLROOT=$(TOOLROOT) LDSCRIPT=$(LDSCRIPT)

.PHONY: all stage patch sysex load store size clean
all: patch

# Always a clean build: the staged copies are new files each time.
stage:
	rm -rf $(STAGE) $(BUILDDIR)
	mkdir -p $(STAGE) $(BUILDDIR)
	cp *.hpp *.cpp *.c *.h *.inc $(STAGE)/
	cp -r $(ROOT)/engine $(STAGE)/engine
	cp -r $(ROOT)/lich $(STAGE)/lich
	cp -r $(ROOT)/third_party/eurorack/beads $(STAGE)/beads
	cp -r $(ROOT)/third_party/eurorack/stmlib $(STAGE)/stmlib

patch sysex load store: stage
	$(MAKE) $(OWL_ARGS) $(if $(SLOT),SLOT=$(SLOT)) $@

size: patch
	$(TOOLROOT)arm-none-eabi-size $(BUILDDIR)/patch.elf
	@$(TOOLROOT)arm-none-eabi-nm --print-size --size-sort -C $(BUILDDIR)/patch.elf | tail -12
	@python3 -c "import os; s=os.path.getsize('$(BUILDDIR)/patch.bin'); print('patch.bin %d bytes (limit 147456): %s' % (s, 'OK' if s <= 147456 else 'TOO BIG'))"

clean:
	rm -rf $(STAGE) $(BUILDDIR)
