// Cortex-M4 benchmark of the Pearls engine, for the m4cycles QEMU plugin.
// Each configuration renders kSeconds of audio; the plugin attributes the
// cycles spent inside Engine::Process to that configuration's phase.
//
// Memory layout as on the Lich: allocations of 60 kB and more (the delay and
// reverb buffers) go to "SDRAM", where the plugin adds the FMC latencies;
// the rest (processor state) to internal RAM.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "engine/pearls_engine.h"

using namespace pearls;

// Bump allocators: internal SRAM and external "SDRAM" (PSRAM at 0x60000000,
// where the plugin applies SDRAM latencies).
extern "C" char __ext_start;
static char sram_pool[600 * 1024] __attribute__((aligned(8)));
static size_t sram_used, ext_used;
static size_t sdram_threshold = 60 * 1024;  // allocations >= this go external

static void* Allocate(size_t size) {
  size = (size + 7) & ~size_t(7);
  if (size >= sdram_threshold) {
    void* p = &__ext_start + ext_used;
    ext_used += size;
    return p;
  }
  if (sram_used + size > sizeof(sram_pool)) {
    printf("SRAM pool exhausted\n");
    exit(2);
  }
  void* p = sram_pool + sram_used;
  sram_used += size;
  return p;
}
// The engine allocates with malloc (as on the Lich, where it is the OWL heap).
extern "C" void* malloc(size_t size) { return Allocate(size); }
extern "C" void free(void*) { }
extern "C" void* calloc(size_t n, size_t size) {
  void* p = Allocate(n * size);
  memset(p, 0, n * size);
  return p;
}
extern "C" void* realloc(void*, size_t size) { return Allocate(size); }
void* operator new(size_t size) { return Allocate(size); }
void* operator new[](size_t size) { return Allocate(size); }
void operator delete(void*) noexcept { }
void operator delete[](void*) noexcept { }
void operator delete(void*, size_t) noexcept { }
void operator delete[](void*, size_t) noexcept { }

static inline void Mark(int phase) {
  *reinterpret_cast<volatile uint32_t*>(0x203ff000 + 4 * phase) = 0;
}

static const size_t kBlock = 32;  // OWL default block size
static const float kSeconds = 1.0f;
static const size_t kInputFrames = 48000;
static float input_l[kInputFrames], input_r[kInputFrames];

struct Config {
  const char* name;
  int quality;
  bool mono;
  bool delay;
  bool synth;
  float density;
  float randomness;
  float size;
  bool freeze;
  float pitch;
  float time;  // 0: the default
};

static void Run(int phase, const Config& cfg) {
  sram_used = ext_used = 0;
  alignas(Engine) static char engine_storage[sizeof(Engine)];
  Engine* engine = new (engine_storage) Engine;
  engine->Init();
#ifdef MAX_GRAINS
  engine->set_max_grains(MAX_GRAINS);
#endif
  Controls c;
  c.SetDefaults();
  c.quality = cfg.quality;
  c.mono = cfg.mono;
  c.synth = cfg.synth;
  c.size = cfg.delay ? 1.0f : cfg.size;
  c.density = cfg.density;
  c.time_random = c.size_random = c.shape_random = c.pitch_random =
      cfg.randomness;
  c.pitch = cfg.pitch;
  if (cfg.time != 0.0f) c.time = cfg.time;
  c.feedback = 0.5f;
  c.dry_wet = 0.8f;
#ifdef REVERB
  c.reverb = REVERB;
#else
  c.reverb = 0.6f;
#endif
  c.gain = 40;  // manual gain: skip the automatic gain measurement

  float out_l[kBlock], out_r[kBlock];
  uint8_t gate[kBlock];
  uint8_t freeze[kBlock];
  std::fill(&freeze[0], &freeze[kBlock], cfg.freeze ? 1 : 0);
  const size_t total = size_t(kSeconds * Engine::kSampleRate);
  // Let the configuration fade in and the buffer fill (not measured).
  size_t pos = 0;
  for (size_t n = 0; n < 96000; n += kBlock, pos = (pos + kBlock) % kInputFrames) {
    engine->Process(c, &input_l[pos], &input_r[pos], freeze, NULL, out_l, out_r,
                    gate, kBlock);
  }
  float peak = 0.0f;
  for (size_t n = 0; n < total; n += kBlock, pos = (pos + kBlock) % kInputFrames) {
    Mark(phase);
    engine->Process(c, &input_l[pos], &input_r[pos], freeze, NULL, out_l, out_r,
                    gate, kBlock);
    Mark(0);
    for (size_t i = 0; i < kBlock; ++i) peak = std::max(peak, fabsf(out_l[i]));
  }
  printf("config %d: %-28s sram %6u ext %7u peak %.3f\n", phase,
         cfg.name, unsigned(sram_used), unsigned(ext_used), double(peak));
  engine->~Engine();
}

int main() {
  float phase = 0.0f;
  uint32_t seed = 1;
  for (size_t i = 0; i < kInputFrames; ++i) {
    phase += 220.0f / 48000.0f;
    if (phase >= 1.0f) phase -= 1.0f;
    seed = seed * 1664525u + 1013904223u;
    input_l[i] = 0.5f * sinf(6.2831853f * phase) +
                 0.01f * (float(seed >> 8) / 16777216.0f - 0.5f);
    input_r[i] = -input_l[i];
  }
  printf("sizeof(Engine) %u\n", unsigned(sizeof(Engine)));
  static const Config configs[] = {
    { "hifi stereo granular", QUALITY_DIGITAL, false, false, false, 0.95f, 0.3f, 0.6f, false, 7.0f },
    { "hifi stereo delay", QUALITY_DIGITAL, false, true, false, 0.95f, 0.3f, 0.6f, false, 7.0f },
    { "hifi mono granular", QUALITY_DIGITAL, true, false, false, 0.95f, 0.3f, 0.6f, false, 7.0f },
    { "cold stereo granular", QUALITY_VINTAGE_DIGITAL, false, false, false, 0.95f, 0.3f, 0.6f, false, 7.0f },
    { "cold stereo delay", QUALITY_VINTAGE_DIGITAL, false, true, false, 0.95f, 0.3f, 0.6f, false, 7.0f },
    { "sunny stereo granular", QUALITY_CLEAN_TAPE, false, false, false, 0.95f, 0.3f, 0.6f, false, 7.0f },
    { "sunny stereo delay", QUALITY_CLEAN_TAPE, false, true, false, 0.95f, 0.3f, 0.6f, false, 7.0f },
    { "scorched stereo granular", QUALITY_DIRTY_TAPE, false, false, false, 0.95f, 0.3f, 0.6f, false, 7.0f },
    { "scorched stereo delay", QUALITY_DIRTY_TAPE, false, true, false, 0.95f, 0.3f, 0.6f, false, 7.0f },
    { "hifi granular moderate", QUALITY_DIGITAL, false, false, false, 0.75f, 0.0f, 0.6f, false, 7.0f },
    { "hifi granular size 0.95", QUALITY_DIGITAL, false, false, false, 0.95f, 0.3f, 0.95f, false, 7.0f },
    { "hifi granular size 0.8", QUALITY_DIGITAL, false, false, false, 0.95f, 0.3f, 0.8f, false, 7.0f },
    { "sunny granular size 0.95", QUALITY_CLEAN_TAPE, false, false, false, 0.95f, 0.3f, 0.95f, false, 7.0f },
    { "hifi stereo delay frozen", QUALITY_DIGITAL, false, true, false, 0.95f, 0.3f, 0.6f, true, 7.0f },
    { "hifi delay unison", QUALITY_DIGITAL, false, true, false, 0.95f, 0.3f, 0.6f, false, 0.0f },
    { "hifi delay sparse", QUALITY_DIGITAL, false, true, false, 0.3f, 0.3f, 0.6f, false, 7.0f },
    { "synth", QUALITY_DIGITAL, false, false, true, 0.95f, 0.3f, 0.6f, false, 7.0f },
    // Knobs at noon, no randomness: as first measured on a Lich.
    { "hifi delay at noon", QUALITY_DIGITAL, false, true, false, 0.5f, 0.0f, 0.5f, false, 0.0f },
    { "hifi granular at noon", QUALITY_DIGITAL, false, false, false, 0.5f, 0.0f, 0.5f, false, 0.0f },
    { "cold delay at noon", QUALITY_VINTAGE_DIGITAL, false, true, false, 0.5f, 0.0f, 0.5f, false, 0.0f },
    { "sunny delay at noon", QUALITY_CLEAN_TAPE, false, true, false, 0.5f, 0.0f, 0.5f, false, 0.0f },
    { "scorched delay at noon", QUALITY_DIRTY_TAPE, false, true, false, 0.5f, 0.0f, 0.5f, false, 0.0f },
    // TIME mid cross-fade (multiplier 8.5: both taps of each pair) and on
    // a multiple (9: one).
    { "hifi delay time 0.5", QUALITY_DIGITAL, false, true, false, 0.5f, 0.0f, 0.5f, false, 0.0f, 0.5f },
    { "hifi delay time 8/15", QUALITY_DIGITAL, false, true, false, 0.5f, 0.0f, 0.5f, false, 0.0f, 8.0f / 15.0f },
  };
  const char* only = NULL;
  (void)only;
#ifdef ONLY
  for (size_t i = ONLY - 1; i < ONLY; ++i) {
#else
  for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); ++i) {
#endif
    Run(int(i) + 1, configs[i]);
  }
  printf("done\n");
  return 0;
}
