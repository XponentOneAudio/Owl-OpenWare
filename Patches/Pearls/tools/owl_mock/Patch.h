// A desktop stand-in for OwlProgram's Patch API, enough to run
// owl/PearlsPatch.hpp in tools/patch_test.cc. Behaviour follows OwlProgram
// (LibSource/Patch.h, Resource.cpp) and OpenWare (ProgramManager.cpp).
#ifndef OWL_MOCK_PATCH_H_
#define OWL_MOCK_PATCH_H_

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

enum PatchParameterId {
  PARAMETER_A, PARAMETER_B, PARAMETER_C, PARAMETER_D, PARAMETER_E,
  PARAMETER_F, PARAMETER_G, PARAMETER_H
};
enum PatchButtonId {
  BYPASS_BUTTON = 0, PUSHBUTTON = 1, GREEN_BUTTON, RED_BUTTON,
  BUTTON_1 = 4, BUTTON_2, BUTTON_3, BUTTON_4
};
#define BUTTON_A BUTTON_1
#define BUTTON_B BUTTON_2
enum PatchChannelId { LEFT_CHANNEL = 0, RIGHT_CHANNEL = 1 };

class AudioBuffer {
 public:
  explicit AudioBuffer(int size) : l_(size), r_(size) { }
  float* getSamples(int channel) { return channel ? &r_[0] : &l_[0]; }
  int getSize() const { return int(l_.size()); }
  void clear() {
    std::fill(l_.begin(), l_.end(), 0.0f);
    std::fill(r_.begin(), r_.end(), 0.0f);
  }
 private:
  std::vector<float> l_, r_;
};

// The firmware side, as the test drives it.
struct MockOwl {
  float parameters[8] = { 0.5f, 0.5f, 0.75f, 0.5f, 0, 0, 0, 0 };
  std::map<int, int> buttons_out;       // setButton values
  std::map<int, int> button_changes;    // count of setButton calls
  float cv_out[8] = { 0 };
  int elapsed_cycles = 0;
  // Resources in "flash". Reads are counted to check the chunking.
  std::map<std::string, std::vector<uint8_t>> resources;
  int resource_reads = 0;
  size_t largest_read = 0;
};
MockOwl& Owl();

class Resource {
 public:
  bool hasData() const { return data != NULL; }
  void* getData() { return data; }
  size_t getSize() const { return size; }
  static Resource* open(const char* name);
  static Resource* load(const char* name);
  static void destroy(Resource* resource);
  size_t read(void* dest, size_t len, size_t offset = 0);
 protected:
  Resource(const char* name, size_t size, uint8_t* data)
      : name(name), size(size), data(data), allocated(false) { }
  const char* name;  // as OwlProgram: a pointer, used again by read()
  size_t size;
  uint8_t* data;
  bool allocated;
};

class Patch {
 public:
  virtual ~Patch() { }
  void registerParameter(PatchParameterId, const char*) { }
  float getParameterValue(PatchParameterId pid) {
    return Owl().parameters[pid];
  }
  void setParameterValue(PatchParameterId pid, float value) {
    Owl().cv_out[pid] = value;
  }
  void setButton(PatchButtonId bid, uint16_t value, uint16_t samples = 0) {
    Owl().buttons_out[bid] = value;
    ++Owl().button_changes[bid];
  }
  float getSampleRate() { return 48000.0f; }
  int getElapsedCycles() { return Owl().elapsed_cycles; }
  virtual void buttonChanged(PatchButtonId bid, uint16_t value,
                             uint16_t samples) { }
  virtual void processAudio(AudioBuffer& audio) = 0;
};

#endif  // OWL_MOCK_PATCH_H_
