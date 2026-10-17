// Impulse response of the engine's reverb (dry signal only, so the output
// is the dry impulse plus the reverb), written as raw float to stdout.
// tools/reverb_compare.py compares the 48 kHz and 24 kHz reverbs.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "engine/pearls_engine.h"
using namespace pearls;
int main(int argc, char** argv) {
  Engine e;
  e.Init();
  Controls c;
  c.SetDefaults();
  c.dry_wet = 0.0f;
  c.reverb = argc > 1 ? atof(argv[1]) : 0.7f;
  c.quality = argc > 2 ? atoi(argv[2]) : QUALITY_DIGITAL;
  c.gain = 20;
  float l[32], r[32];
  const int kTotal = 48000 * 6;
  for (int n = 0; n < kTotal; n += 32) {
    for (int i = 0; i < 32; ++i) {
      // A burst of noise at 1 s (the gain control and fades have settled).
      static uint32_t rng = 1;
      rng = rng * 1664525u + 1013904223u;
      const int t = n + i;
      l[i] = r[i] = (t >= 48000 && t < 48000 + 480)
          ? 0.5f * (float(rng >> 8) / 8388608.0f - 1.0f) : 0.0f;
    }
    e.Process(c, l, r, NULL, NULL, l, r, NULL, 32);
    fwrite(l, sizeof(float), 32, stdout);
  }
  return 0;
}
