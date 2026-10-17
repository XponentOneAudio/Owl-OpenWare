// Renders fixed test scenarios through the engine and writes the raw float
// output to stdout. Built twice (upstream per-sample grains and the Lich
// block rendering) by tools/check_block_rendering.sh to compare the two.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "engine/pearls_engine.h"

using namespace pearls;

namespace {

struct Scenario {
  int quality;
  bool mono;
  float size;
  float density;
  float randomness;
  float pitch;
  float feedback;
  bool toggle_freeze;
  bool seed_gate;
  float reverse;
};

void Render(const Scenario& sc, std::vector<float>* output) {
  Engine engine;
  engine.Init();
  Controls c;
  c.SetDefaults();
  c.quality = sc.quality;
  c.mono = sc.mono;
  c.size = sc.size;
  c.time = sc.mono ? 0.1f : 0.3f;  // within what 6 s of audio has recorded
  c.density = sc.density;
  c.time_random = c.size_random = c.shape_random = c.pitch_random =
      sc.randomness;
  c.pitch = sc.pitch;
  c.feedback = sc.feedback;
  c.dry_wet = 1.0f;
  c.reverb = 0.3f;
  c.reverse = sc.reverse;
  c.gain = 20;

  const size_t kBlock = 32;
  const size_t kTotal = 48000 * 6;
  float in_l[kBlock], in_r[kBlock], out_l[kBlock], out_r[kBlock];
  uint8_t freeze[kBlock], seed[kBlock], gate[kBlock];
  float phase = 0.0f;
  uint32_t rng = 12345;
  for (size_t n = 0; n < kTotal; n += kBlock) {
    for (size_t i = 0; i < kBlock; ++i) {
      const size_t t = n + i;
      phase += (110.0f + 50.0f * float((t / 24000) % 4)) / 48000.0f;
      if (phase >= 1.0f) phase -= 1.0f;
      rng = rng * 1664525u + 1013904223u;
      in_l[i] = 0.4f * sinf(6.2831853f * phase) +
                0.05f * (float(rng >> 8) / 16777216.0f - 0.5f);
      in_r[i] = 0.4f * sinf(6.2831853f * phase * 1.5f);
      // Freeze held from 2.0 s to 3.3 s and 4.1 s to 4.6 s.
      freeze[i] = sc.toggle_freeze &&
          ((t >= 96000 && t < 158400) || (t >= 196800 && t < 220800));
      // Seed gate: 8 ms pulses every 77 ms.
      seed[i] = sc.seed_gate && (t % 3700) < 384;
    }
    engine.Process(c, in_l, in_r, freeze, sc.seed_gate ? seed : NULL,
                   out_l, out_r, gate, kBlock);
    for (size_t i = 0; i < kBlock; ++i) {
      output->push_back(out_l[i]);
      output->push_back(out_r[i]);
      output->push_back(gate[i] ? 1.0f : 0.0f);
    }
  }
}

}  // namespace

int main() {
  const Scenario scenarios[] = {
    // quality, mono, size, density, random, pitch, fb, freeze, seed, reverse
    { QUALITY_DIGITAL, false, 0.6f, 0.95f, 0.3f, 7.0f, 0.5f, true, false, 0.0f },
    { QUALITY_DIGITAL, false, 0.95f, 0.9f, 0.5f, -5.0f, 0.3f, true, false, 0.3f },
    { QUALITY_DIGITAL, true, 0.2f, 0.85f, 0.2f, 12.0f, 0.6f, false, false, 0.0f },
    { QUALITY_VINTAGE_DIGITAL, false, 0.7f, 0.1f, 0.4f, 0.0f, 0.4f, true, false, 0.0f },
    { QUALITY_CLEAN_TAPE, false, 0.5f, 0.75f, 0.0f, 3.0f, 0.7f, false, true, 0.5f },
    { QUALITY_CLEAN_TAPE, true, 0.9f, 0.95f, 0.6f, -12.0f, 0.2f, true, false, 0.0f },
    { QUALITY_DIRTY_TAPE, false, 0.4f, 0.6f, -0.5f, 5.0f, 0.5f, true, true, 0.0f },
    { QUALITY_DIRTY_TAPE, true, 0.8f, 0.3f, 0.3f, 19.0f, 0.9f, false, true, 0.2f },
  };
  for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
    std::vector<float> output;
    Render(scenarios[i], &output);
    fwrite(&output[0], sizeof(float), output.size(), stdout);
  }
  return 0;
}
