// Decodes a .syx file the way OpenWare does (FirmwareLoader.hpp and
// MidiHandler.cpp, with OpenWare's own sysex.c and crc32.c) and compares
// the result with the original binary.
//
//   cc -I<OpenWare>/Source check_sysex.c <OpenWare>/Source/sysex.c \
//      <OpenWare>/Source/crc32.c -o check_sysex
//   check_sysex file.syx original.bin

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crc32.h"
#include "sysex.h"

static uint32_t decode_int(uint8_t* data) {
  uint8_t buf[4];
  sysex_to_data(data, buf, 5);
  return buf[3] | (buf[2] << 8) | (buf[1] << 16) | ((uint32_t)buf[0] << 24);
}

int main(int argc, char** argv) {
  FILE* f = fopen(argv[1], "rb");
  FILE* g = fopen(argv[2], "rb");
  if (!f || !g) return 2;
  static uint8_t syx[4 << 20], orig[4 << 20], buffer[4 << 20];
  size_t n = fread(syx, 1, sizeof(syx), f);
  size_t m = fread(orig, 1, sizeof(orig), g);
  size_t size = 0, index = 0, package = 0;
  uint32_t crc = 0;
  int ready = 0, messages = 0;
  const char* final = "none";
  for (size_t i = 0; i < n;) {
    if (syx[i] != 0xf0) { printf("not SysEx at %zu\n", i); return 1; }
    size_t j = i;
    while (syx[j] != 0xf7) ++j;
    uint8_t* data = &syx[i];
    size_t len = j - i + 1;
    if (len > 256) { printf("message too long: %zu\n", len); return 1; }
    ++messages;
    if (data[1] != 0x7d || data[2] != 0x52) { printf("bad header\n"); return 1; }
    if (data[3] == 0x10) {
      // handleFirmwareUploadCommand(data + 1, size - 2)
      uint8_t* d = data + 1;
      size_t length = len - 2;
      size_t offset = 3;
      size_t idx = decode_int(d + offset);
      offset += 5;
      if (idx == 0) {
        size = decode_int(d + offset);
        index = 0; package = 1; crc = 0; ready = 0;
      } else if (idx != package) {
        printf("out of sequence\n"); return 1;
      } else if (index < size) {
        size_t l = sysex_to_data(d + offset, buffer + index, length - offset);
        crc = crc32(buffer + index, l, crc);
        index += l;
        ++package;
      } else if (index == size) {
        if (decode_int(d + offset) != crc) { printf("bad checksum\n"); return 1; }
        ready = 1;
      } else {
        printf("invalid size\n"); return 1;
      }
    } else if (data[3] == 0x12) {
      final = "RUN";
    } else if (data[3] == 0x11) {
      final = "STORE";
      printf("store slot %u\n", decode_int(data + 4));
    } else if (data[3] == 0x17) {
      final = "SAVE";
      printf("save as \"%s\"\n", (char*)(data + 4));
    } else {
      printf("unexpected command 0x%02x\n", data[3]); return 1;
    }
    i = j + 1;
  }
  if (!ready || index != m || memcmp(buffer, orig, m)) {
    printf("FAIL: ready %d, %zu of %zu bytes\n", ready, index, m);
    return 1;
  }
  printf("OK: %d messages, %zu bytes, CRC 0x%08x, then %s\n", messages, index,
         crc, final);
  return 0;
}
