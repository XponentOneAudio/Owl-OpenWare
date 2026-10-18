// Desktop tests of the Lich front end (lich/lich_pearls.cc): pages, pickup,
// button gestures, LEDs, outputs and the CPU governor, driven like the OWL
// patch drives it (blocks of 32 frames at 48 kHz).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "engine/pearls_engine.h"
#include "lich/lich_pearls.h"

using namespace pearls;
using namespace pearls::lich;

namespace {

int failures = 0;

// The wavetables, for builds without them (BEADS_EXTERNAL_WAVETABLES): the
// data of the resource file given on the command line, as on the Lich.
std::vector<uint8_t> g_waves;

#define CHECK(cond) do { \
    if (!(cond)) { \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures; \
    } \
  } while (0)

#define CHECK_NEAR(a, b, tol) do { \
    const double _a = (a), _b = (b); \
    if (fabs(_a - _b) > (tol)) { \
      printf("  FAIL %s:%d: %s = %g, expected %g\n", __FILE__, __LINE__, \
             #a, _a, _b); \
      ++failures; \
    } \
  } while (0)

const size_t kBlock = 32;
const float kRate = 48000.0f;

// A simulated Lich: panel state, audio in, and what came out.
struct Rig {
  LichPearls pearls;
  Inputs in;
  Outputs out;
  float phase;
  float peak;
  bool nan;
  int gate_rises;
  bool gate_previous;
  int led_changes[NUM_BUTTONS];
  bool led_previous[NUM_BUTTONS];
  bool led_seen_on[NUM_BUTTONS];
  float load;  // reported to the governor when >= 0

  Rig() : phase(0.0f), peak(0.0f), nan(false), gate_rises(0),
          gate_previous(false), load(-1.0f) {
    memset(&in, 0, sizeof(in));
    for (int k = 0; k < NUM_KNOBS; ++k) in.knob[k] = 0.5f;
    for (int b = 0; b < NUM_BUTTONS; ++b) {
      in.edge_frame[b] = -1;
      led_changes[b] = 0;
      led_previous[b] = false;
      led_seen_on[b] = false;
    }
    in.knob[KNOB_C] = 0.75f;
    if (!g_waves.empty()) {
      pearls.set_wavetables(&g_waves[0], g_waves.size());
    }
    if (!pearls.Init(kRate)) {
      printf("Init failed\n");
      exit(1);
    }
  }

  void Run(float seconds) {
    const size_t blocks = size_t(seconds * kRate / kBlock);
    for (size_t n = 0; n < blocks; ++n) {
      float l[kBlock], r[kBlock];
      for (size_t i = 0; i < kBlock; ++i) {
        phase += 220.0f / kRate;
        if (phase >= 1.0f) phase -= 1.0f;
        l[i] = 0.4f * sinf(6.2831853f * phase);
        r[i] = 0.4f * sinf(6.2831853f * phase * 1.5f);
      }
      pearls.Process(in, l, r, l, r, kBlock, &out);
      for (int b = 0; b < NUM_BUTTONS; ++b) in.pulse[b] = false;
      for (size_t i = 0; i < kBlock; ++i) {
        if (!(fabsf(l[i]) < 10.0f) || !(fabsf(r[i]) < 10.0f)) nan = true;
        peak = std::max(peak, std::max(fabsf(l[i]), fabsf(r[i])));
      }
      if (out.gate && !gate_previous) ++gate_rises;
      gate_previous = out.gate;
      for (int b = 0; b < NUM_BUTTONS; ++b) {
        if (out.led[b] != led_previous[b]) ++led_changes[b];
        led_previous[b] = out.led[b];
        led_seen_on[b] = led_seen_on[b] || out.led[b];
      }
      if (load >= 0.0f) pearls.ReportLoad(load);
    }
  }

  void ResetCounters() {
    peak = 0.0f;
    gate_rises = 0;
    for (int b = 0; b < NUM_BUTTONS; ++b) {
      led_changes[b] = 0;
      led_seen_on[b] = false;
    }
  }

  void Press(Button b, float seconds) {
    in.button[b] = true;
    Run(seconds);
    in.button[b] = false;
    Run(0.05f);
  }

  void Pulse(Button b) {
    in.pulse[b] = true;
    Run(kBlock / kRate);
  }

  // SEED held, FREEZE pressed: next page.
  void NextPage() {
    in.button[BUTTON_SEED] = true;
    Run(0.3f);
    in.button[BUTTON_FREEZE] = true;
    Run(0.05f);
    in.button[BUTTON_FREEZE] = false;
    Run(0.05f);
    in.button[BUTTON_SEED] = false;
    Run(0.05f);
  }

  void GoToPage(int page) {
    while (pearls.ui_state().page != page) NextPage();
  }

  // Moves a knob there, through the value it must catch (as a hand would).
  void Turn(Knob k, float value) {
    const float start = in.knob[k];
    for (int i = 1; i <= 20; ++i) {
      in.knob[k] = start + (value - start) * float(i) / 20.0f;
      Run(0.005f);
    }
  }
};

void TestMainPage() {
  printf("main page: knobs follow at once, 1 V/oct pitch\n");
  Rig rig;
  rig.in.knob[KNOB_A] = 0.2f;
  rig.in.knob[KNOB_B] = 0.7f;
  rig.in.knob[KNOB_C] = 0.9f;
  rig.in.knob[KNOB_D] = 0.6f;
  rig.Run(0.01f);
  const Controls& c = rig.pearls.controls();
  CHECK(rig.pearls.ui_state().page == PAGE_GRAINS);
  CHECK_NEAR(c.time, 0.2, 1e-6);
  CHECK_NEAR(c.size, 0.7, 1e-6);
  CHECK_NEAR(c.density, 0.9, 1e-6);
  CHECK_NEAR(c.pitch, 12.0, 1e-4);  // +1 V = +1 octave
  rig.in.knob[KNOB_D] = 1.0f;
  rig.Run(0.01f);
  CHECK_NEAR(c.pitch, 48.0, 1e-4);  // clamped
  // Defaults of the other pages.
  CHECK_NEAR(c.shape, 0.5, 1e-6);
  CHECK_NEAR(c.feedback, 0.0, 1e-6);
  CHECK_NEAR(c.dry_wet, 0.5, 1e-6);
  CHECK_NEAR(c.reverb, 0.0, 1e-6);
  CHECK_NEAR(c.time_random, 0.0, 1e-6);
  CHECK(c.quality == QUALITY_DIGITAL);
  CHECK(c.gain == 0);
  CHECK(!c.mono);
  CHECK(!c.synth);
}

void TestPagesAndPickup() {
  printf("pages: SEED held + FREEZE changes page, knobs pick up\n");
  Rig rig;
  rig.in.knob[KNOB_A] = 0.3f;
  rig.Run(0.1f);
  rig.NextPage();
  CHECK(rig.pearls.ui_state().page == PAGE_MIX);
  CHECK(!rig.pearls.ui_state().frozen);   // the FREEZE press didn't freeze
  const Controls& c = rig.pearls.controls();
  // Knob A is at 0.3, SHAPE at 0.5: not caught, SHAPE unchanged, and TIME
  // keeps its value.
  rig.in.knob[KNOB_A] = 0.32f;
  rig.Run(0.05f);
  CHECK(!rig.pearls.ui_state().knob_caught[KNOB_A]);
  CHECK_NEAR(c.shape, 0.5, 1e-6);
  CHECK_NEAR(c.time, 0.3, 1e-6);
  // Through 0.5: caught.
  rig.Turn(KNOB_A, 0.8f);
  CHECK(rig.pearls.ui_state().knob_caught[KNOB_A]);
  CHECK_NEAR(c.shape, 0.8, 1e-6);
  CHECK_NEAR(c.time, 0.3, 1e-6);
  // Feedback, dry/wet, reverb.
  rig.Turn(KNOB_B, 0.0f);  // feedback is 0: caught on the way
  rig.Turn(KNOB_B, 0.4f);
  CHECK_NEAR(c.feedback, 0.4, 1e-6);
  rig.Turn(KNOB_D, 0.0f);
  rig.Turn(KNOB_D, 0.6f);
  CHECK_NEAR(c.reverb, 0.6, 1e-6);

  // Random page: attenuverters, noon = none.
  rig.NextPage();
  CHECK(rig.pearls.ui_state().page == PAGE_RANDOM);
  rig.Turn(KNOB_A, 0.5f);
  rig.Turn(KNOB_A, 1.0f);
  CHECK_NEAR(c.time_random, 1.0, 1e-5);
  rig.Turn(KNOB_A, 0.51f);
  CHECK_NEAR(c.time_random, 0.0, 1e-6);   // dead zone
  rig.Turn(KNOB_A, 0.0f);
  CHECK_NEAR(c.time_random, -1.0, 1e-5);

  // Extras page: reverse, spread, scale.
  rig.NextPage();
  CHECK(rig.pearls.ui_state().page == PAGE_EXTRAS);
  rig.Turn(KNOB_A, 0.0f);
  rig.Turn(KNOB_A, 0.7f);
  CHECK_NEAR(c.reverse, 0.7, 1e-6);
  rig.Turn(KNOB_B, 0.0f);
  rig.Turn(KNOB_B, 0.4f);
  CHECK_NEAR(c.spread, 0.4, 1e-6);
  rig.Turn(KNOB_C, 0.06f);
  CHECK(c.scale == SCALE_OFF);
  rig.Turn(KNOB_C, 0.30f);
  CHECK(c.scale == SCALE_MAJOR);
  rig.Turn(KNOB_C, 0.43f);
  CHECK(c.scale == SCALE_MINOR);
  rig.Turn(KNOB_C, 0.99f);
  CHECK(c.scale == SCALE_OCTAVES);

  // Setup page.
  rig.NextPage();
  CHECK(rig.pearls.ui_state().page == PAGE_SETUP);
  rig.Turn(KNOB_A, 0.125f);
  rig.Turn(KNOB_A, 0.9f);
  CHECK(c.quality == QUALITY_DIRTY_TAPE);
  rig.Turn(KNOB_A, 0.6f);
  CHECK(c.quality == QUALITY_CLEAN_TAPE);
  rig.Turn(KNOB_A, 0.3f);
  CHECK(c.quality == QUALITY_VINTAGE_DIGITAL);
  rig.Turn(KNOB_A, 0.1f);
  CHECK(c.quality == QUALITY_DIGITAL);
  rig.Turn(KNOB_B, 0.0f);
  CHECK(c.gain == 0);
  rig.Turn(KNOB_B, 1.0f);
  CHECK(c.gain == 255);
  rig.Turn(KNOB_C, 0.15f);
  CHECK(!c.mono && !c.synth);
  rig.Turn(KNOB_C, 0.5f);
  CHECK(c.mono && !c.synth);
  rig.Turn(KNOB_C, 0.9f);
  CHECK(c.synth == rig.pearls.synth_available());
  rig.Turn(KNOB_C, 0.5f);
  CHECK(c.mono);

  // Wraps to the main page; its knobs must pick up again.
  rig.NextPage();
  CHECK(rig.pearls.ui_state().page == PAGE_GRAINS);
  CHECK(!rig.pearls.ui_state().knob_caught[KNOB_A]);   // knob A at 0.0
  CHECK_NEAR(c.time, 0.3, 1e-6);
  rig.Turn(KNOB_A, 0.6f);
  CHECK(rig.pearls.ui_state().knob_caught[KNOB_A]);
  CHECK_NEAR(c.time, 0.6, 1e-6);
  // Settings kept.
  CHECK_NEAR(c.shape, 0.8, 1e-6);
  CHECK(c.mono);
}

void TestFreeze() {
  printf("freeze: a press toggles, a long press is momentary, gates too\n");
  Rig rig;
  rig.Run(0.2f);
  rig.Press(BUTTON_FREEZE, 0.1f);
  rig.Run(0.1f);
  CHECK(rig.pearls.ui_state().frozen);
  CHECK(rig.out.led[BUTTON_FREEZE]);
  rig.Press(BUTTON_FREEZE, 0.1f);
  rig.Run(0.1f);
  CHECK(!rig.pearls.ui_state().frozen);
  // Long press: frozen while held only.
  rig.in.button[BUTTON_FREEZE] = true;
  rig.Run(1.0f);
  CHECK(rig.pearls.ui_state().frozen);
  rig.in.button[BUTTON_FREEZE] = false;
  rig.Run(0.1f);
  CHECK(!rig.pearls.ui_state().frozen);
  // A trigger shorter than a block (seen as a pulse): toggles.
  rig.Pulse(BUTTON_FREEZE);
  rig.Run(0.05f);
  CHECK(rig.pearls.ui_state().frozen);
  rig.Pulse(BUTTON_FREEZE);
  rig.Run(0.05f);
  CHECK(!rig.pearls.ui_state().frozen);
}

void TestSeed() {
  printf("seed, button mode: Beads' SEED button\n");
  {
    Rig rig;
    rig.in.knob[KNOB_C] = 0.5f;   // density at noon: no grains on their own
    rig.in.knob[KNOB_B] = 0.3f;
    rig.Run(1.0f);
    rig.ResetCounters();
    rig.Run(1.0f);
    CHECK(rig.gate_rises == 0);
    CHECK(rig.pearls.ui_state().seed_locked);   // Beads starts locked
    for (int i = 0; i < 5; ++i) {
      rig.Press(BUTTON_SEED, 0.02f);
      rig.Run(0.2f);
    }
    CHECK(rig.gate_rises >= 4);
    CHECK(!rig.pearls.ui_state().seed_locked);  // a press unlocks
    rig.ResetCounters();
    for (int i = 0; i < 5; ++i) {
      rig.Pulse(BUTTON_SEED);
      rig.Run(0.2f);
    }
    CHECK(rig.gate_rises >= 4);
    rig.Press(BUTTON_SEED, 4.2f);               // 4 s press: locked again
    CHECK(rig.pearls.ui_state().seed_locked);
    rig.Press(BUTTON_SEED, 0.05f);
    CHECK(!rig.pearls.ui_state().seed_locked);
    rig.NextPage();                             // SEED + FREEZE locks
    CHECK(rig.pearls.ui_state().seed_locked);
    CHECK(rig.pearls.ui_state().page == PAGE_MIX);
  }

  printf("seed, clock mode: Beads' SEED input\n");
  {
    Rig rig;
    rig.in.knob[KNOB_B] = 0.3f;
    rig.GoToPage(PAGE_SETUP);
    rig.Turn(KNOB_D, 0.15f);
    rig.Turn(KNOB_D, 0.5f);
    rig.GoToPage(PAGE_GRAINS);
    rig.Turn(KNOB_C, 1.0f);      // every clock pulse makes a grain
    // No edges for 4 s: unpatched, grains run free (as Pearls on Nebulae).
    rig.Run(5.0f);
    rig.ResetCounters();
    rig.Run(1.0f);
    CHECK(rig.gate_rises > 0);
    for (int i = 0; i < 6; ++i) {
      rig.Pulse(BUTTON_SEED);
      rig.Run(0.25f);
    }
    CHECK(rig.gate_rises >= 4);
    CHECK(rig.pearls.ui_state().seed_locked);
    rig.Press(BUTTON_SEED, 4.2f);  // 4 s press toggles the lock
    CHECK(!rig.pearls.ui_state().seed_locked);
    rig.Press(BUTTON_SEED, 4.2f);
    CHECK(rig.pearls.ui_state().seed_locked);
  }
}

void TestTap() {
  printf("seed, tap mode: tap tempo clocks the grains\n");
  Rig rig;
  rig.in.knob[KNOB_B] = 0.3f;
  rig.GoToPage(PAGE_SETUP);
  rig.Turn(KNOB_D, 0.15f);
  rig.Turn(KNOB_D, 0.9f);
  rig.GoToPage(PAGE_GRAINS);
  rig.Turn(KNOB_C, 1.0f);       // a grain on every beat
  rig.Run(1.0f);
  for (int i = 0; i < 4; ++i) {  // taps 0.25 s apart: 240 BPM
    rig.Press(BUTTON_SEED, 0.02f);
    rig.Run(0.23f);
  }
  rig.ResetCounters();
  rig.Run(2.0f);
  // The clock goes on after the taps: about 8 beats in 2 s.
  CHECK(rig.gate_rises >= 6 && rig.gate_rises <= 10);
  rig.Press(BUTTON_SEED, 1.2f);  // a 1 s press stops it
  rig.ResetCounters();
  rig.Run(2.0f);
  CHECK(rig.gate_rises <= 9);
}

// A WAV file in memory.
std::vector<uint8_t> MakeWav(int channels, int bits, bool ieee, int rate,
                             int frames) {
  std::vector<uint8_t> w;
  auto put = [&w](uint32_t v, int n) {
    for (int i = 0; i < n; ++i) w.push_back(uint8_t(v >> (8 * i)));
  };
  const int bytes = bits / 8;
  const int data = frames * channels * bytes;
  w.insert(w.end(), {'R', 'I', 'F', 'F'});
  put(36 + 8 + 4 + data, 4);  // with a LIST chunk
  w.insert(w.end(), {'W', 'A', 'V', 'E', 'L', 'I', 'S', 'T'});
  put(4, 4);
  w.insert(w.end(), {'I', 'N', 'F', 'O'});
  w.insert(w.end(), {'f', 'm', 't', ' '});
  put(16, 4);
  put(ieee ? 3 : 1, 2);
  put(channels, 2);
  put(rate, 4);
  put(rate * channels * bytes, 4);
  put(channels * bytes, 2);
  put(bits, 2);
  w.insert(w.end(), {'d', 'a', 't', 'a'});
  put(data, 4);
  for (int f = 0; f < frames; ++f) {
    for (int c = 0; c < channels; ++c) {
      const float v = 0.5f * sinf(6.2831853f * 440.0f * f / rate) *
          (c ? -1.0f : 1.0f);
      if (ieee) {
        uint32_t u;
        memcpy(&u, &v, 4);
        put(u, 4);
      } else if (bits == 8) {
        put(uint32_t(int(v * 127.0f) + 128), 1);
      } else {
        const int64_t full = int64_t(v * float(1u << (bits - 1)));
        put(uint32_t(full), bytes);
      }
    }
  }
  return w;
}

void TestWav() {
  printf("WAV reader: formats and errors\n");
  const int formats[][3] = {
    { 16, 0, 2 }, { 24, 0, 2 }, { 32, 0, 1 }, { 8, 0, 1 }, { 32, 1, 2 },
  };
  for (const auto& f : formats) {
    std::vector<uint8_t> w = MakeWav(f[2], f[0], f[1], 44100, 1000);
    WavInfo info;
    CHECK(ParseWav(&w[0], w.size(), &info));
    CHECK(info.frames == 1000);
    CHECK_NEAR(info.sample_rate, 44100, 0);
    float l, r;
    ReadWavFrame(&info, 25, &l, &r);   // a quarter period at 440 Hz
    const float expected = 0.5f * sinf(6.2831853f * 440.0f * 25 / 44100);
    CHECK_NEAR(l, expected, f[0] == 8 ? 0.01 : 1e-4);
    CHECK_NEAR(r, f[2] == 2 ? -expected : expected, f[0] == 8 ? 0.01 : 1e-4);
  }
  std::vector<uint8_t> w = MakeWav(2, 16, false, 48000, 100);
  WavInfo info;
  CHECK(!ParseWav(&w[0], 20, &info));        // truncated
  w[8] = 'X';
  CHECK(!ParseWav(&w[0], w.size(), &info));  // not WAVE
  CHECK(!ParseWav(NULL, 0, &info));
}

void TestSamples() {
  printf("samples: the extras page loads a slot into the buffer, frozen\n");
  Rig rig;
  rig.in.knob[KNOB_A] = 0.0f;
  rig.Run(0.5f);
  CHECK(rig.pearls.TakeSampleRequest() == 0);
  rig.GoToPage(PAGE_EXTRAS);
  rig.Turn(KNOB_D, 0.0f);
  rig.Turn(KNOB_D, 3.5f / 9.0f);   // slot 3
  const int slot = rig.pearls.TakeSampleRequest();
  CHECK(slot == 3);
  CHECK(rig.pearls.TakeSampleRequest() == 0);
  rig.pearls.SampleReading();
  rig.Run(0.1f);
  std::vector<uint8_t> w = MakeWav(2, 16, false, 44100, 44100);
  CHECK(rig.pearls.LoadSample(&w[0], w.size()));
  CHECK(rig.pearls.sample_busy());
  rig.Run(3.0f);
  CHECK(!rig.pearls.sample_busy());
  CHECK(rig.pearls.ui_state().frozen);  // loaded = frozen
  // The sample plays (the input is a different tone; it isn't recorded).
  rig.GoToPage(PAGE_GRAINS);
  rig.Turn(KNOB_C, 0.9f);
  rig.ResetCounters();
  rig.Run(1.0f);
  CHECK(rig.peak > 0.05f);
  // One FREEZE press releases it.
  rig.Press(BUTTON_FREEZE, 0.05f);
  rig.Run(0.1f);
  CHECK(!rig.pearls.ui_state().frozen);
  // A bad file or a missing resource: an error, nothing loads.
  rig.GoToPage(PAGE_EXTRAS);
  rig.Turn(KNOB_D, 3.5f / 9.0f);
  rig.Turn(KNOB_D, 5.5f / 9.0f);
  CHECK(rig.pearls.TakeSampleRequest() == 5);
  w[8] = 'X';
  CHECK(!rig.pearls.LoadSample(&w[0], w.size()));
  CHECK(!rig.pearls.sample_busy());
  rig.pearls.SampleFailed();
  rig.Run(0.5f);
  CHECK(!rig.pearls.ui_state().frozen);
}

void TestLeds() {
  printf("LEDs: page blinks and beacon\n");
  Rig rig;
  rig.in.knob[KNOB_C] = 0.5f;
  rig.Run(0.5f);
  rig.NextPage();
  rig.ResetCounters();
  rig.Run(2.5f);
  // Away from the main page, the SEED LED pulses the page number.
  CHECK(rig.led_changes[BUTTON_SEED] >= 4);
  CHECK(!rig.out.led[BUTTON_FREEZE] || rig.pearls.ui_state().frozen);
  rig.GoToPage(PAGE_GRAINS);
  rig.Run(1.0f);
  rig.ResetCounters();
  rig.Run(2.0f);
  // Main page, locked and idle: dim (PWM) SEED LED.
  CHECK(rig.led_seen_on[BUTTON_SEED]);
}

void TestWavetables() {
  printf("wavetables: checked, synth mode only with them\n");
  Rig rig;
#ifdef BEADS_EXTERNAL_WAVETABLES
  CHECK(!rig.pearls.set_wavetables(NULL, 0));
  std::vector<uint8_t> junk(99840, 0);
  CHECK(!rig.pearls.set_wavetables(&junk[0], junk.size()));
  CHECK(!rig.pearls.synth_available());
  if (!g_waves.empty()) {
    CHECK(rig.pearls.set_wavetables(&g_waves[0], g_waves.size()));
    CHECK(rig.pearls.synth_available());
  }
#endif
  rig.GoToPage(PAGE_SETUP);
  rig.Turn(KNOB_C, 0.15f);
  rig.Turn(KNOB_C, 0.9f);
  CHECK(rig.pearls.controls().synth == rig.pearls.synth_available());
}

void TestColdDigitalStreaming() {
  printf("cold digital: 16-frame blocks stream cleanly through 32 kHz\n");
  Engine engine;
  engine.Init();
  Controls c;
  c.SetDefaults();
  c.quality = QUALITY_VINTAGE_DIGITAL;
  c.dry_wet = 0.0f;   // the dry path: 12-bit at 32 kHz, then back
  c.gain = 20;        // manual gain: no level changes
  const int kTotal = 48000 * 3;
  std::vector<float> out(kTotal);
  float in_l[32], in_r[32], out_l[32], out_r[32];
  for (int n = 0; n < kTotal; n += 32) {
    for (int i = 0; i < 32; ++i) {
      in_l[i] = in_r[i] = 0.02f * sinf(6.2831853f * 1000.0f * (n + i) / 48000);
    }
    engine.Process(c, in_l, in_r, NULL, NULL, out_l, out_r, NULL, 32);
    std::copy(out_l, out_l + 32, &out[n]);
  }
  CHECK(engine.streaming_underflows() == 0 || Engine::kBlockSize % 3 == 0);
  // The last second against the best-fitting 1 kHz sine.
  double ss = 0, sc = 0, energy = 0;
  const int from = kTotal - 48000;
  for (int t = from; t < kTotal; ++t) {
    ss += out[t] * sin(2 * M_PI * 1000.0 * t / 48000);
    sc += out[t] * cos(2 * M_PI * 1000.0 * t / 48000);
  }
  ss *= 2.0 / 48000;
  sc *= 2.0 / 48000;
  double residual = 0;
  for (int t = from; t < kTotal; ++t) {
    const double fit = ss * sin(2 * M_PI * 1000.0 * t / 48000) +
        sc * cos(2 * M_PI * 1000.0 * t / 48000);
    residual += (out[t] - fit) * (out[t] - fit);
    energy += fit * fit;
  }
  const double snr = 10 * log10(energy / (residual + 1e-30));
  printf("  level %.4f, signal to noise %.1f dB\n",
         sqrt(ss * ss + sc * sc), snr);
  CHECK(sqrt(ss * ss + sc * sc) > 0.005);
  CHECK(snr > 40.0);
}

void TestGovernor() {
  printf("governor: grain voices follow the CPU load\n");
  Rig rig;
  rig.Run(0.1f);
  CHECK(rig.pearls.ui_state().max_grains == LichPearls::kDefaultMaxGrains);
  rig.load = 0.5f;
  rig.Run(5.0f);
  CHECK(rig.pearls.ui_state().max_grains == 32);
  rig.load = 0.9f;
  rig.Run(0.5f);
  CHECK(rig.pearls.ui_state().max_grains == 4);
  rig.load = 0.78f;   // between the thresholds: unchanged
  rig.Run(1.0f);
  CHECK(rig.pearls.ui_state().max_grains == 4);
  rig.load = 0.6f;
  rig.Run(0.55f);
  CHECK(rig.pearls.ui_state().max_grains > 4);
  CHECK(rig.pearls.ui_state().max_grains < 12);
  rig.pearls.set_grain_ceiling(10);
  rig.Run(3.0f);
  CHECK(rig.pearls.ui_state().max_grains == 10);
}

void TestAudio() {
  printf("audio: all qualities and modes produce sound, no NaN\n");
  for (int quality_zone = 0; quality_zone < 4; ++quality_zone) {
    for (int mono = 0; mono < 2; ++mono) {
      Rig rig;
      rig.in.knob[KNOB_A] = 0.1f;   // recent audio
      rig.in.knob[KNOB_C] = 0.9f;
      rig.GoToPage(PAGE_MIX);
      rig.Turn(KNOB_C, 0.5f);
      rig.Turn(KNOB_C, 1.0f);       // fully wet
      rig.GoToPage(PAGE_SETUP);
      rig.Turn(KNOB_A, 0.125f);
      rig.Turn(KNOB_A, 0.125f + 0.25f * float(quality_zone));
      rig.Turn(KNOB_C, 0.15f);
      rig.Turn(KNOB_C, mono ? 0.5f : 0.2f);
      rig.GoToPage(PAGE_GRAINS);
      rig.Turn(KNOB_C, 0.9f);
      rig.Run(2.0f);
      rig.ResetCounters();
      rig.Run(2.0f);
      CHECK(rig.peak > 0.05f);
      CHECK(!rig.nan);
      CHECK(rig.gate_rises > 5);
      CHECK(rig.out.cv[0] >= 0.0f && rig.out.cv[0] <= 1.0f);
      CHECK(rig.out.cv[1] > 0.0f && rig.out.cv[1] <= 1.0f);
    }
  }
  // Delay mode and synth mode.
  Rig rig;
  rig.in.knob[KNOB_B] = 1.0f;
  rig.Run(3.0f);
  rig.ResetCounters();
  rig.Run(1.0f);
  CHECK(rig.peak > 0.05f);
  CHECK(!rig.nan);
  rig.GoToPage(PAGE_SETUP);
  rig.Turn(KNOB_C, 0.15f);
  rig.Turn(KNOB_C, 0.9f);
  rig.GoToPage(PAGE_GRAINS);
  rig.Run(2.0f);
  rig.ResetCounters();
  rig.Run(1.0f);
  CHECK(rig.pearls.controls().synth == rig.pearls.synth_available());
  CHECK(rig.peak > 0.05f);
  CHECK(!rig.nan);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    // An OpenWare resource (tools/make_wavetable_resource.py): 32-byte
    // header, then the data.
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
      printf("cannot open %s\n", argv[1]);
      return 1;
    }
    std::vector<uint8_t> file(200000);
    file.resize(fread(&file[0], 1, file.size(), f));
    fclose(f);
    g_waves.assign(file.begin() + 32, file.end());
  }
  TestMainPage();
  TestPagesAndPickup();
  TestFreeze();
  TestSeed();
  TestTap();
  TestWav();
  TestSamples();
  TestLeds();
  TestWavetables();
  TestColdDigitalStreaming();
  TestGovernor();
  TestAudio();
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all Lich tests passed\n");
  return 0;
}
