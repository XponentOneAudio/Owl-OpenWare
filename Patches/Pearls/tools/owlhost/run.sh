#!/bin/sh
# run.sh [patch.bin]: runs the patch binary under QEMU with host.c standing
# in for the firmware.
set -e
cd "$(dirname "$0")"
T=${TOOLROOT:-}
B=../../build/owlhost
P=$(realpath ${1:-../../prebuilt/pearls-lich.bin})
W=$(realpath ../../build/waves.bin)
ARCH="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
mkdir -p $B
${T}arm-none-eabi-gcc $ARCH -O1 -g -c startup.c -o $B/startup.o
${T}arm-none-eabi-gcc $ARCH -O1 -g $HOSTFLAGS -c host.c -o $B/host.o
${T}arm-none-eabi-gcc $ARCH -c blobs.S -o $B/blobs.o "-DPATCH_BIN=\"$P\"" "-DWAVES_BIN=\"$W\""
${T}arm-none-eabi-gcc $ARCH -T host.ld -nostartfiles --specs=rdimon.specs -o $B/host.elf \
  $B/startup.o $B/host.o $B/blobs.o -Wl,--start-group -lc -lm -lgcc -lrdimon -Wl,--end-group
timeout 900 qemu-system-arm -M mps2-an386 -cpu cortex-m4 -nographic -monitor none \
  -serial none -semihosting-config enable=on,target=native -kernel $B/host.elf
