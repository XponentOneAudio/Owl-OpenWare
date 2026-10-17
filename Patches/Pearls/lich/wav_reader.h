// Copyright 2026 The Pearls (Beads for Nebulae) contributors.
// MIT licence: see lich_pearls.h.
//
// WAV files in memory (the Lich loads samples from OWL resources): parsing,
// and frame-by-frame reading for Engine::StartLoad, without a copy.

#ifndef PEARLS_LICH_WAV_READER_H_
#define PEARLS_LICH_WAV_READER_H_

#include <cstddef>
#include <cstdint>

namespace pearls {
namespace lich {

struct WavInfo {
  const uint8_t* data;   // first frame
  size_t frames;
  float sample_rate;
  int channels;          // as in the file; the first two are used
  int bits;              // 8, 16, 24, 32 (integer or float)
  bool ieee_float;
  size_t block_align;
};

// Parses a RIFF/WAVE file: PCM 8/16/24/32-bit, or 32-bit float.
bool ParseWav(const void* file, size_t size, WavInfo* info);

// Reads one frame as -1..1 floats (mono: both channels the same). Matches
// Engine::FrameReader, with a WavInfo as the context.
void ReadWavFrame(const void* info, size_t frame, float* l, float* r);

}  // namespace lich
}  // namespace pearls

#endif  // PEARLS_LICH_WAV_READER_H_
