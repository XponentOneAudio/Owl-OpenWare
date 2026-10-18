// Copyright 2026 The Pearls (Beads for Nebulae) contributors.
// MIT licence: see lich/lich_pearls.h.
//
// Pearls on the Befaco Lich (OWL2): the OWL patch. The panel logic and the
// engine are in lich/lich_pearls.{h,cc}; this class only moves values
// between OpenWare and them.

#ifndef __PearlsPatch_hpp__
#define __PearlsPatch_hpp__

#include <algorithm>
#include <cstring>

#include "Patch.h"
#include "message.h"
#include "lich/lich_pearls.h"

class PearlsPatch : public Patch {
 public:
  PearlsPatch() {
    // Knob names for the OWL web interface (page 1).
    registerParameter(PARAMETER_A, "Time");
    registerParameter(PARAMETER_B, "Size");
    registerParameter(PARAMETER_C, "Density");
    registerParameter(PARAMETER_D, "Pitch");
    registerParameter(PARAMETER_F, "Random>");
    registerParameter(PARAMETER_G, "Level>");
    for (int b = 0; b < pearls::lich::NUM_BUTTONS; ++b) {
      level_[b] = false;
      pulse_[b] = false;
      led_[b] = -1;
    }
    gate_ = -1;
    sample_resource_ = NULL;
    sample_data_ = NULL;
    sample_size_ = 0;
    sample_read_ = 0;
    // The wavetable synth's tables, stored once in the Lich's flash (see
    // README.md). Without them, everything but synth mode works.
    Calibrate();
    waves_ = Resource::load("pearls-waves");
    if (waves_ && waves_->hasData()) {
      pearls_.set_wavetables(waves_->getData(), waves_->getSize());
    }
    ok_ = pearls_.Init(getSampleRate());
    cycles_per_block_ = 0.0f;
    overloaded_blocks_ = 0;
    skip_block_ = false;
    report_blocks_ = 0;
    load_sum_ = 0.0f;
    load_peak_ = 0.0f;
  }

  ~PearlsPatch() {
    FinishSampleRead();
    FreeSample();
    pearls_.set_wavetables(NULL, 0);
    Resource::destroy(waves_);
  }

  void buttonChanged(PatchButtonId bid, uint16_t value, uint16_t samples) override {
    int b = bid == BUTTON_A ? 0 : bid == BUTTON_B ? 1 : -1;
    if (b < 0) {
      return;
    }
    const bool high = value != 0;
    // The firmware reports each button once per block, with its level at
    // the end of the block: an unchanged level means a short pulse.
    if (high == level_[b]) {
      pulse_[b] = true;
    }
    level_[b] = high;
  }

  void processAudio(AudioBuffer& buffer) override {
    const int size = buffer.getSize();
    float* l = buffer.getSamples(LEFT_CHANNEL);
    float* r = buffer.getSamples(RIGHT_CHANNEL);
    if (!ok_ || skip_block_) {
      // Overload guard (below): a block of silence that costs nothing.
      buffer.clear();
      skip_block_ = false;
      return;
    }
    pearls::lich::Inputs in;
    in.knob[0] = getParameterValue(PARAMETER_A);
    in.knob[1] = getParameterValue(PARAMETER_B);
    in.knob[2] = getParameterValue(PARAMETER_C);
    in.knob[3] = getParameterValue(PARAMETER_D);
    for (int b = 0; b < pearls::lich::NUM_BUTTONS; ++b) {
      in.button[b] = level_[b];
      in.edge_frame[b] = -1;
      in.pulse[b] = pulse_[b];
      pulse_[b] = false;
    }
    pearls::lich::Outputs out;
    pearls_.Process(in, l, r, l, r, size, &out);

    setButton(BUTTON_A, out.led[0] ? 4095 : 0, 0);
    setButton(BUTTON_B, out.led[1] ? 4095 : 0, 0);
    if (int(out.gate) != gate_) {
      gate_ = out.gate;
      setButton(PUSHBUTTON, out.gate ? 4095 : 0, 0);
    }
    setParameterValue(PARAMETER_F, out.cv[0]);
    setParameterValue(PARAMETER_G, out.cv[1]);

    UpdateSampleLoad();

    // CPU governor: OpenWare resets the cycle counter before each block.
    if (cycles_per_block_ == 0.0f) {
      cycles_per_block_ = float(size) * kCpuHz / getSampleRate();
    }
    const float load = float(getElapsedCycles()) / cycles_per_block_;
    pearls_.ReportLoad(load);
    Report(load, size);

    // Overload guard: a patch that never finishes its blocks in time
    // starves the firmware's lower priority tasks, its watchdog resets the
    // module, and the firmware then refuses to start the patch (E). After
    // a run of late blocks, skip one: the firmware catches up while the
    // governor sheds grains.
    overloaded_blocks_ = load > 1.0f ? overloaded_blocks_ + 1 : 0;
    if (overloaded_blocks_ >= kOverloadBlocks) {
      overloaded_blocks_ = 0;
      skip_block_ = true;
    }
  }

 private:
  static constexpr float kCpuHz = 168e6f;  // the Lich's STM32F427

  // Measured once at startup, for the CPU model (tools/m4sim): the extra
  // cycles of a read from the SDRAM over one from the CCM, at random
  // addresses and sequentially, the cycles per word of a memcpy() from the
  // SDRAM, and the cycles per iteration of a loop of 4 dependent fused
  // multiply-adds. Reported with the load (send_sysex.py --info).
  struct Calibration {
    int random_x10, sequential_x10, copy_x10, fma_x10;
  };

  static __attribute__((noinline)) uint32_t ReadRandom(
      const uint32_t* p, int shift, int n) {
    uint32_t x = 1;
    uint32_t sum = 0;
    for (int i = 0; i < n; ++i) {
      x = x * 1664525u + 1013904223u;
      sum += p[x >> shift];
    }
    return sum;
  }

  static __attribute__((noinline)) uint32_t ReadSequential(
      const uint32_t* p, uint32_t mask, int n) {
    uint32_t sum = 0;
    for (int i = 0; i < n; ++i) {
      sum += p[uint32_t(i) & mask];
    }
    return sum;
  }

  static __attribute__((noinline)) float Fma(float a, int n) {
    const float b = 0.9999f;
    const float c = 0.0001f;
    for (int i = 0; i < n; ++i) {
      a = a * b + c;
      a = a * b + c;
      a = a * b + c;
      a = a * b + c;
    }
    return a;
  }

  // The fewest cycles of 3 runs (interrupts only add).
  template<typename F>
  __attribute__((noinline)) int Cycles(F f) {
    int best = 0x7fffffff;
    for (int k = 0; k < 3; ++k) {
      const int start = getElapsedCycles();
      f();
      best = std::min(best, getElapsedCycles() - start);
    }
    return best;
  }

  __attribute__((noinline, optimize("Os"))) void Calibrate() {
    static const int kExtWords = 32768;  // 128 kB: too big for the CCM
    static const int kCcmWords = 1024;
    static const int kN = 4096;
    calibration_ = Calibration();
    uint32_t* ccm = static_cast<uint32_t*>(malloc(kCcmWords * 4));
    uint32_t* ext = static_cast<uint32_t*>(malloc(kExtWords * 4));
    if (ccm && ext) {
      memset(ccm, 0, kCcmWords * 4);
      memset(ext, 0, kExtWords * 4);
      volatile uint32_t sink = 0;
      const int random_ext = Cycles([&] { sink = ReadRandom(ext, 17, kN); });
      const int random_ccm = Cycles([&] { sink = ReadRandom(ccm, 22, kN); });
      const int seq_ext = Cycles([&] {
        sink = ReadSequential(ext, kExtWords - 1, kN); });
      const int seq_ccm = Cycles([&] {
        sink = ReadSequential(ccm, kCcmWords - 1, kN); });
      const int copy = Cycles([&] {
        for (int i = 0; i < 4; ++i) {
          memcpy(ccm, ext + i * 8192, kCcmWords * 4);
        }
        sink = ccm[7]; });
      volatile float fsink = 0.0f;
      const int fma = Cycles([&] { fsink = Fma(1.0f, kN); });
      (void)sink;
      (void)fsink;
      calibration_.random_x10 = (random_ext - random_ccm) * 10 / kN;
      calibration_.sequential_x10 = (seq_ext - seq_ccm) * 10 / kN;
      calibration_.copy_x10 = copy * 10 / (4 * kCcmWords);
      calibration_.fma_x10 = fma * 10 / kN;
    }
    free(ext);
    free(ccm);
  }

  static __attribute__((noinline, optimize("Os"))) char* Append(
      char* p, const char* s) {
    while (*s) *p++ = *s++;
    return p;
  }

  static __attribute__((noinline, optimize("Os"))) char* AppendInt(
      char* p, int v) {
    if (v < 0) {
      *p++ = '-';
      v = -v;
    }
    char digits[12];
    int n = 0;
    do {
      digits[n++] = char('0' + v % 10);
      v /= 10;
    } while (v);
    while (n) *p++ = digits[--n];
    return p;
  }

  static __attribute__((noinline, optimize("Os"))) char* AppendTenths(
      char* p, int v_x10) {
    p = AppendInt(p, v_x10 / 10);
    *p++ = '.';
    return AppendInt(p, (v_x10 < 0 ? -v_x10 : v_x10) % 10);
  }

  // Twice a second: the calibration, and the load (average and peak, in
  // percent) and grain limit since the last report.
  __attribute__((noinline, optimize("Os"))) void Report(
      float load, int size) {
    load_sum_ += load;
    load_peak_ = std::max(load_peak_, load);
    if (++report_blocks_ * size < int(getSampleRate()) / 2) {
      return;
    }
    char* p = message_;
    p = Append(p, "rnd ");
    p = AppendTenths(p, calibration_.random_x10);
    p = Append(p, " seq ");
    p = AppendTenths(p, calibration_.sequential_x10);
    p = Append(p, " cpy ");
    p = AppendTenths(p, calibration_.copy_x10);
    p = Append(p, " fma ");
    p = AppendTenths(p, calibration_.fma_x10);
    p = Append(p, " cpu ");
    p = AppendInt(p, int(100.0f * load_sum_ / float(report_blocks_)));
    *p++ = '/';
    p = AppendInt(p, int(100.0f * load_peak_));
    p = Append(p, " g ");
    p = AppendInt(p, pearls_.ui_state().max_grains);
    *p = '\0';
    debugMessage(message_);
    report_blocks_ = 0;
    load_sum_ = 0.0f;
    load_peak_ = 0.0f;
  }

  // Samples: resources "pearls-1.wav" to "pearls-8.wav", read from flash a
  // few kB per block (a whole file at once would stall the audio).
  void UpdateSampleLoad() {
    static const size_t kChunk = 4096;
    static const size_t kMaxSize = 6 << 20;  // the SDRAM has about 7 MB free
    if (sample_resource_) {
      const size_t n = std::min(kChunk, sample_size_ - sample_read_);
      if (sample_resource_->read(sample_data_ + sample_read_, n,
                                 sample_read_) != n) {
        FinishSampleRead();
        FreeSample();
        pearls_.SampleFailed();
        return;
      }
      sample_read_ += n;
      if (sample_read_ == sample_size_) {
        FinishSampleRead();
        if (!pearls_.LoadSample(sample_data_, sample_size_)) {
          FreeSample();
        }
      }
    } else if (sample_data_) {
      if (!pearls_.sample_busy()) {
        FreeSample();  // copied into the buffer
      }
    } else {
      const int slot = pearls_.TakeSampleRequest();
      if (slot <= 0) {
        return;
      }
      // A member: the Resource keeps a pointer to its name for read().
      strcpy(sample_name_, "pearls-0.wav");
      sample_name_[7] = char('0' + slot);
      sample_resource_ = Resource::open(sample_name_);
      sample_size_ = sample_resource_ ? sample_resource_->getSize() : 0;
      if (sample_size_ && sample_size_ <= kMaxSize) {
        sample_data_ = new uint8_t[sample_size_];
      }
      if (!sample_data_) {
        FinishSampleRead();
        pearls_.SampleFailed();
        return;
      }
      sample_read_ = 0;
      pearls_.SampleReading();
    }
  }

  void FinishSampleRead() {
    Resource::destroy(sample_resource_);
    sample_resource_ = NULL;
  }

  void FreeSample() {
    delete[] sample_data_;
    sample_data_ = NULL;
  }

  Resource* waves_;
  Resource* sample_resource_;
  char sample_name_[16];
  uint8_t* sample_data_;
  size_t sample_size_;
  size_t sample_read_;

  pearls::lich::LichPearls pearls_;
  bool level_[pearls::lich::NUM_BUTTONS];
  bool pulse_[pearls::lich::NUM_BUTTONS];
  int led_[pearls::lich::NUM_BUTTONS];
  int gate_;
  bool ok_;
  float cycles_per_block_;
  Calibration calibration_;
  int report_blocks_;
  float load_sum_;
  float load_peak_;
  char message_[64];
  static const int kOverloadBlocks = 16;
  int overloaded_blocks_;
  bool skip_block_;
};

#endif  // __PearlsPatch_hpp__
