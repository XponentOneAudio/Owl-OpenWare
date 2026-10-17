// Desktop test of the OWL patch itself (owl/PearlsPatch.hpp), against a
// stand-in for the OwlProgram API (tools/owl_mock/Patch.h). Run with the
// wavetable resource: patch_test build/pearls-waves.res

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "Patch.h"

// For the checks: the patch's front end.
#define private public
#include "owl/PearlsPatch.hpp"
#undef private
#include "engine/pearls_engine.h"  // Controls, for the checks

namespace {

int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures; \
    } \
  } while (0)

std::vector<uint8_t> Wav(int frames, int rate) {
  std::vector<uint8_t> w;
  auto put = [&w](uint32_t v, int n) {
    for (int i = 0; i < n; ++i) w.push_back(uint8_t(v >> (8 * i)));
  };
  w.insert(w.end(), {'R', 'I', 'F', 'F'});
  put(36 + frames * 4, 4);
  w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put(16, 4); put(1, 2); put(2, 2); put(rate, 4); put(rate * 4, 4);
  put(4, 2); put(16, 2);
  w.insert(w.end(), {'d', 'a', 't', 'a'});
  put(frames * 4, 4);
  for (int f = 0; f < frames; ++f) {
    const int16_t v = int16_t(12000 * sinf(6.2831853f * 330.0f * f / rate));
    put(uint16_t(v), 2);
    put(uint16_t(-v), 2);
  }
  return w;
}

// The firmware: one block of 32 frames. Buttons are reported once per
// block with their level at the end of the block (OpenWare
// ProgramManager.cpp onProgramReady), as the Lich ORs buttons and gates.
struct Firmware {
  PearlsPatch* patch;
  bool level[2];
  float peak;
  int gate_rises;
  int last_gate;

  Firmware() : patch(NULL), peak(0.0f), gate_rises(0), last_gate(0) {
    level[0] = level[1] = false;
  }

  void Block(bool b0, bool b1, bool pulse0 = false, bool pulse1 = false) {
    const bool want[2] = { b0, b1 };
    const bool pulse[2] = { pulse0, pulse1 };
    for (int b = 0; b < 2; ++b) {
      if (want[b] != level[b] || pulse[b]) {
        patch->buttonChanged(b ? BUTTON_B : BUTTON_A, want[b] ? 4095 : 0,
                             0);
        if (b == 0) {
          patch->buttonChanged(PUSHBUTTON, want[b] ? 4095 : 0, 0);
        }
        level[b] = want[b];
      }
    }
    AudioBuffer buffer(32);
    static float phase = 0.0f;
    for (int i = 0; i < 32; ++i) {
      phase += 220.0f / 48000.0f;
      if (phase > 1.0f) phase -= 1.0f;
      buffer.getSamples(0)[i] = 0.3f * sinf(6.2831853f * phase);
      buffer.getSamples(1)[i] = 0.3f * sinf(6.2831853f * phase);
    }
    patch->processAudio(buffer);
    for (int i = 0; i < 32; ++i) {
      peak = std::max(peak, fabsf(buffer.getSamples(0)[i]));
    }
    const int g = Owl().buttons_out[PUSHBUTTON];
    if (g && !last_gate) ++gate_rises;
    last_gate = g;
  }

  void Run(float seconds, bool b0 = false, bool b1 = false) {
    for (int n = 0; n < int(seconds * 1500.0f); ++n) Block(b0, b1);
  }

  void NextPage() {
    Run(0.3f, false, true);
    Run(0.05f, true, true);
    Run(0.05f, false, true);
    Run(0.05f);
  }

  void Turn(int k, float to) {
    const float from = Owl().parameters[k];
    for (int i = 1; i <= 20; ++i) {
      Owl().parameters[k] = from + (to - from) * i / 20.0f;
      Run(0.005f);
    }
  }
};

}  // namespace

MockOwl& Owl() {
  static MockOwl owl;
  return owl;
}

Resource* Resource::open(const char* name) {
  auto it = Owl().resources.find(name);
  if (it == Owl().resources.end()) return NULL;
  // Stored in SPI flash: not memory mapped, read() reads it.
  return new Resource(name, it->second.size(), NULL);
}

Resource* Resource::load(const char* name) {
  Resource* r = open(name);
  if (r) {
    r->data = new uint8_t[r->size];
    memcpy(r->data, &Owl().resources[name][0], r->size);
    r->allocated = true;
  }
  return r;
}

void Resource::destroy(Resource* r) {
  if (r && r->allocated) delete[] r->data;
  delete r;
}

size_t Resource::read(void* dest, size_t len, size_t offset) {
  auto it = Owl().resources.find(name);  // through the stored name pointer
  if (it == Owl().resources.end() || (offset & 3)) return 0;
  const size_t n = std::min(len, it->second.size() - offset);
  memcpy(dest, &it->second[offset], n);
  ++Owl().resource_reads;
  Owl().largest_read = std::max(Owl().largest_read, n);
  return n;
}

int main(int argc, char** argv) {
  if (argc > 1) {
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    std::vector<uint8_t> res(200000);
    res.resize(fread(&res[0], 1, res.size(), f));
    fclose(f);
    Owl().resources["pearls-waves"].assign(res.begin() + 32, res.end());
  }
  Owl().resources["pearls-2.wav"] = Wav(48000, 44100);

  printf("patch: starts, makes sound, gate and CV outputs\n");
  PearlsPatch patch;
  Firmware fw;
  fw.patch = &patch;
  CHECK(patch.ok_);
  CHECK(patch.pearls_.synth_available() == (argc > 1));
  Owl().parameters[PARAMETER_C] = 0.9f;
  Owl().parameters[PARAMETER_A] = 0.1f;
  fw.Run(3.0f);
  CHECK(fw.peak > 0.05f);
  CHECK(fw.gate_rises > 10);
  CHECK(Owl().cv_out[PARAMETER_F] >= 0.0f && Owl().cv_out[PARAMETER_F] <= 1.0f);
  CHECK(Owl().cv_out[PARAMETER_G] > 0.0f);

  printf("patch: buttons, short triggers and LEDs\n");
  fw.Run(0.05f, true, false);   // FREEZE pressed
  fw.Run(0.05f);
  CHECK(patch.pearls_.ui_state().frozen);
  CHECK(Owl().buttons_out[BUTTON_A] != 0);   // FREEZE LED on
  fw.Block(false, false, true, false);       // a trigger inside one block
  fw.Run(0.05f);
  CHECK(!patch.pearls_.ui_state().frozen);
  fw.NextPage();
  CHECK(patch.pearls_.ui_state().page == pearls::lich::PAGE_MIX);

  printf("patch: CPU governor reads the cycle counter\n");
  Owl().elapsed_cycles = int(0.95f * 32 * 3750);
  fw.Run(0.3f);
  CHECK(patch.pearls_.ui_state().max_grains == 4);
  Owl().elapsed_cycles = int(0.5f * 32 * 3750);
  fw.Run(5.0f);
  CHECK(patch.pearls_.ui_state().max_grains == 32);

  printf("patch: a sample slot reads the resource in chunks\n");
  while (patch.pearls_.ui_state().page != pearls::lich::PAGE_EXTRAS) {
    fw.NextPage();
  }
  fw.Turn(PARAMETER_D, 0.0f);
  fw.Turn(PARAMETER_D, 2.5f / 9.0f);   // slot 2
  fw.Run(3.0f);
  CHECK(Owl().resource_reads > 20);
  CHECK(Owl().largest_read <= 4096);
  CHECK(patch.sample_data_ == NULL);    // freed after the transfer
  CHECK(patch.sample_resource_ == NULL);
  CHECK(patch.pearls_.ui_state().frozen);
  // A missing slot: an error, nothing breaks.
  fw.Turn(PARAMETER_D, 5.5f / 9.0f);
  fw.Run(0.5f);
  CHECK(patch.sample_data_ == NULL);
  CHECK(patch.pearls_.ui_state().frozen);  // the loaded sample stays

  if (argc > 1) {
    printf("patch: synth mode with the wavetable resource\n");
    while (patch.pearls_.ui_state().page != pearls::lich::PAGE_SETUP) {
      fw.NextPage();
    }
    fw.Turn(PARAMETER_C, 0.15f);
    fw.Turn(PARAMETER_C, 0.9f);
    CHECK(patch.pearls_.controls().synth);
    fw.peak = 0.0f;
    fw.Run(2.0f);
    CHECK(fw.peak > 0.05f);
  }

  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all patch tests passed\n");
  return 0;
}
