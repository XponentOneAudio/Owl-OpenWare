// Desktop tests of the Lich front end (lich/lich_pearls.cc): pages, pickup,
// button gestures, LEDs, outputs and the CPU governor, driven like the OWL
// patch drives it (blocks of 32 frames at 48 kHz).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "engine/pearls_engine.h"
#include "lich/lich_pearls.h"

using namespace pearls;
using namespace pearls::lich;

namespace {

int failures = 0;

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
  CHECK(c.synth);  // built-in wavetables on the desktop
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
    rig.Turn(KNOB_D, 0.25f);
    rig.Turn(KNOB_D, 0.9f);
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
  CHECK(rig.pearls.controls().synth);
  CHECK(rig.peak > 0.05f);
  CHECK(!rig.nan);
}

}  // namespace

int main() {
  TestMainPage();
  TestPagesAndPickup();
  TestFreeze();
  TestSeed();
  TestLeds();
  TestGovernor();
  TestAudio();
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all Lich tests passed\n");
  return 0;
}
