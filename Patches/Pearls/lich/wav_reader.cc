// Copyright 2026 The Pearls (Beads for Nebulae) contributors.
// MIT licence: see lich_pearls.h.
//
// WAV files in memory (after engine/pearls_files.cc's ReadWavFile on the
// Nebulae).

#include "lich/wav_reader.h"

#include <cstring>

// Lich port: see owl/lich_config.h.
#ifndef PEARLS_COLD
#define PEARLS_COLD
#endif

namespace pearls {
namespace lich {

namespace {

inline uint16_t Le16(const uint8_t* p) {
  return uint16_t(p[0] | (p[1] << 8));
}

inline uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
      (uint32_t(p[3]) << 24);
}

inline float Sample(const WavInfo& w, const uint8_t* p) {
  float s;
  if (w.ieee_float) {
    memcpy(&s, p, 4);
  } else if (w.bits == 8) {
    s = (float(p[0]) - 128.0f) / 128.0f;
  } else if (w.bits == 16) {
    s = float(int16_t(Le16(p))) / 32768.0f;
  } else if (w.bits == 24) {
    const int32_t v = int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) |
                              (uint32_t(p[2]) << 24)) >> 8;
    s = float(v) / 8388608.0f;
  } else {
    s = float(int32_t(Le32(p))) / 2147483648.0f;
  }
  // NaN and out of range values from float files.
  if (!(s > -1.0f)) {
    s = s < 0.0f ? -1.0f : 0.0f;
  } else if (s > 1.0f) {
    s = 1.0f;
  }
  return s;
}

}  // namespace

PEARLS_COLD bool ParseWav(const void* file, size_t size, WavInfo* info) {
  const uint8_t* p = static_cast<const uint8_t*>(file);
  if (!p || size < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) {
    return false;
  }
  size_t offset = 12;
  int format = 0;
  bool have_format = false;
  memset(info, 0, sizeof(*info));
  while (offset + 8 <= size) {
    const uint8_t* chunk = p + offset;
    const uint32_t chunk_size = Le32(chunk + 4);
    const size_t body = offset + 8;
    if (!memcmp(chunk, "fmt ", 4) && chunk_size >= 16 &&
        body + chunk_size <= size) {
      format = Le16(chunk + 8);
      info->channels = Le16(chunk + 10);
      info->sample_rate = float(Le32(chunk + 12));
      info->block_align = Le16(chunk + 20);
      info->bits = Le16(chunk + 22);
      if (format == 0xfffe && chunk_size >= 26) {
        format = Le16(chunk + 32);  // WAVE_FORMAT_EXTENSIBLE subformat
      }
      have_format = true;
    } else if (!memcmp(chunk, "data", 4)) {
      if (!have_format) {
        return false;
      }
      // Some writers leave the size at 0 or 0xffffffff: use what's there.
      size_t bytes = size - body;
      if (chunk_size != 0 && chunk_size != 0xffffffff && chunk_size < bytes) {
        bytes = chunk_size;
      }
      const bool pcm = format == 1 && (info->bits == 8 || info->bits == 16 ||
                                       info->bits == 24 || info->bits == 32);
      info->ieee_float = format == 3 && info->bits == 32;
      if ((!pcm && !info->ieee_float) || info->channels < 1 ||
          info->block_align < size_t(info->bits / 8 * info->channels) ||
          info->sample_rate < 1000.0f || info->sample_rate > 768000.0f) {
        return false;
      }
      info->data = p + body;
      info->frames = bytes / info->block_align;
      return info->frames > 0;
    }
    offset = body + chunk_size + (chunk_size & 1);
  }
  return false;
}

void ReadWavFrame(const void* context, size_t frame, float* l, float* r) {
  const WavInfo& w = *static_cast<const WavInfo*>(context);
  const uint8_t* p = w.data + frame * w.block_align;
  *l = Sample(w, p);
  *r = w.channels > 1 ? Sample(w, p + w.bits / 8) : *l;
}

}  // namespace lich
}  // namespace pearls
