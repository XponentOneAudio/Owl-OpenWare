// Copyright 2017 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Delay line.

#ifndef BEADS_DSP_DELAY_LINE_H_
#define BEADS_DSP_DELAY_LINE_H_

#include <algorithm>
#include <cstring>
#include <cassert>

#include "stmlib/stmlib.h"

#include "stmlib/dsp/dsp.h"
#include "stmlib/utils/dsp.h"

#include "beads/dsp/frame.h"
#include "beads/dsp/mu_law.h"

namespace beads {

enum Resolution {
  RESOLUTION_8_BIT,
  RESOLUTION_12_BIT,
  RESOLUTION_16_BIT,
  RESOLUTION_LAST
};

template<Resolution resolution, int num_channels>
struct Item { };

template<>
struct Item<RESOLUTION_16_BIT, 1> {
  void Store(int i, FloatFrame s) {
    lr = stmlib::Clip16(static_cast<int32_t>((s.l + s.r) * 16384.0f));
  }
  inline FloatFrame value(int i) const {
    FloatFrame result;
    result.l = result.r = static_cast<float>(lr) / 32768.0f;
    return result;
  }
  enum { bundle_size = 1 };
  int16_t lr;
};

template<>
struct Item<RESOLUTION_16_BIT, 2> {
  void Store(int i, FloatFrame s) {
    l = stmlib::Clip16(static_cast<int32_t>(s.l * 32768.0f));
    r = stmlib::Clip16(static_cast<int32_t>(s.r * 32768.0f));
  }
  inline FloatFrame value(int i) const {
    FloatFrame result;
    result.l = static_cast<float>(l) / 32768.0f;
    result.r = static_cast<float>(r) / 32768.0f;
    return result;
  }
  enum { bundle_size = 1 };
  int16_t l;
  int16_t r;
};

template<>
struct Item<RESOLUTION_12_BIT, 2> {
  void Store(int i, FloatFrame s) {
    l = stmlib::Clip16(static_cast<int32_t>(s.l * 32768.0f)) >> 4;
    r = stmlib::Clip16(static_cast<int32_t>(s.r * 32768.0f)) >> 4;
  }
  inline FloatFrame value(int i) const {
    FloatFrame result;
    result.l = static_cast<float>(l) / 2048.0f;
    result.r = static_cast<float>(r) / 2048.0f;
    return result;
  }
  enum { bundle_size = 1 };
  int16_t l : 12;
  int16_t r : 12;
} __attribute__((packed));

template<>
struct Item<RESOLUTION_12_BIT, 1> {
  void Store(int i, FloatFrame s) {
    int16_t lr = \
        stmlib::Clip16(static_cast<int32_t>((s.l + s.r) * 16384.0f)) >> 4;
    if (i) {
      odd = lr;
    } else {
      even = lr;
    }
  }
  inline FloatFrame value(int i) const {
    FloatFrame result;
    result.l = result.r = static_cast<float>(i ? odd : even) / 2048.0f;
    return result;
  }
  enum { bundle_size = 2 };
  int16_t odd : 12;
  int16_t even : 12;
} __attribute__((packed));

template<>
struct Item<RESOLUTION_8_BIT, 1> {
  void Store(int i, FloatFrame s) {
    lr = Lin2MuLaw(
        stmlib::Clip16(static_cast<int32_t>((s.l + s.r) * 16384.0f)));
  }
  inline FloatFrame value(int i) const {
    FloatFrame result;
    result.l = result.r = MuLaw2Lin(lr);
    return result;
  }
  enum { bundle_size = 1 };
  uint8_t lr;
};

template<>
struct Item<RESOLUTION_8_BIT, 2> {
  void Store(int i, FloatFrame s) {
    l = Lin2MuLaw(stmlib::Clip16(static_cast<int32_t>(s.l * 32768.0f)));
    r = Lin2MuLaw(stmlib::Clip16(static_cast<int32_t>(s.r * 32768.0f)));
  }
  inline FloatFrame value(int i) const {
    FloatFrame result;
    result.l = MuLaw2Lin(l);
    result.r = MuLaw2Lin(r);
    return result;
  }
  enum { bundle_size = 1 };
  uint8_t l;
  uint8_t r;
};

enum Interpolation {
  INTERPOLATION_ZOH,
  INTERPOLATION_LINEAR,
  INTERPOLATION_HERMITE
};

// Lich port: DelayLine::ReadAt()'s Hermite interpolation, shared with
// DelayLine::ReadHermiteBlock(), and the latter's loop (not a template, so
// that the 8 delay line types share it).
static inline __attribute__((always_inline)) FloatFrame HermiteInterpolate(
    const FloatFrame* x, float t) {
  const FloatFrame c = (x[2] - x[0]) * 0.5f;
  const FloatFrame v = x[1] - x[2];
  const FloatFrame w = c + v;
  const FloatFrame a = w + v + (x[3] - x[1]) * 0.5f;
  const FloatFrame b_neg = w + a;
  return (((a * t) - b_neg) * t + c) * t + x[1];
}

// The longest block (the engine's 16 frames) and the frames read through one
// window: few enough for the stack (the firmware gives the patch 8 kB).
enum {
  kMaxHermiteBlock = 16,
  kHermiteWindowFrames = 48
};

typedef void (*HermiteBlockReader)(
    const void* delay_line,
    const int32_t* position,
    const int* write_ptr,
    const bool* active,
    FloatFrame* out,
    size_t size);

typedef void (*WindowLoader)(
    const void* delay_line, int first, int count, FloatFrame* frames);

// DelayLine::ReadHermiteBlockOf(): the frame index and fraction each sample
// starts from (as ReadAt<INTERPOLATION_HERMITE>), then the interpolation
// over windows of frames that load() copies from the delay line. One window
// when the block reads few enough frames (the case of the delay taps), else
// as many as needed.
__attribute__((noinline, noclone)) inline void HermiteBlock(
    WindowLoader load,
    const void* delay_line,
    int max_delay,
    const int32_t* position,
    const int* write_ptr,
    const bool* active,
    FloatFrame* out,
    size_t size) {
  int index[kMaxHermiteBlock];
  float fractional[kMaxHermiteBlock];
  int lo = max_delay + 4;
  int hi = -1;
  for (size_t s = 0; s < size; ++s) {
    if (!active[s]) {
      continue;
    }
    const int32_t delay_integral = (position[s] >> 10) + max_delay - 1;
    fractional[s] = float(position[s] & 1023) / 1024.0f;
    const int i = (write_ptr[s] + delay_integral) % max_delay;
    index[s] = i;
    lo = std::min(lo, i);
    hi = std::max(hi, i);
  }
  if (hi < 0) {
    return;
  }
  FloatFrame frames[kHermiteWindowFrames];
  int first = 0;
  int count = 0;
  if (hi - lo + 4 <= kHermiteWindowFrames) {
    first = lo;
    count = hi - lo + 4;
    load(delay_line, first, count, frames);
  }
  for (size_t s = 0; s < size; ++s) {
    if (!active[s]) {
      continue;
    }
    const int i = index[s];
    if (i < first || i + 4 > first + count) {
      // A window around frame i: the next samples read close by, before or
      // after it. Frames 0 to max_delay + 3 exist (the interpolation tail).
      first = std::max(0, i - (kHermiteWindowFrames - 4) / 2);
      count = std::min(int(kHermiteWindowFrames), max_delay + 4 - first);
      load(delay_line, first, count, frames);
    }
    out[s] = HermiteInterpolate(&frames[i - first], fractional[s]);
  }
}

template<
    Resolution resolution,
    int max_delay,
    int num_channels,
    size_t bank_size=262144,
    size_t num_banks=3>
class DelayLine {
 public:
  DelayLine() { }
  ~DelayLine() { }
  
  typedef Item<resolution, num_channels> ItemType;

  enum {
    NUM_CHANNELS = num_channels,
    MAX_DELAY = max_delay,
    
    interpolation_tail = 4,
    freeze_tail_size = 256,
  };
  
  void Init(uint8_t* buffer, size_t allocated_size) {
    assert(
        allocated_size >= bank_size * num_banks + freeze_tail_size_in_bytes());
    uint8_t* buffers[num_banks];
    for (size_t i = 0; i < num_banks; ++i) {
      buffers[i] = buffer;
      buffer += bank_size;
    }
    Init(buffers);
  }

  void Init(uint8_t* buffers[]) {
    /*assert((delay_line_size_in_bytes() + freeze_tail_size_in_bytes()) <= \
           bank_size * num_banks);
    if (resolution == RESOLUTION_16_BIT && num_channels == 2) {
      assert(sizeof(ItemType) == 4);
    }
    if (resolution == RESOLUTION_16_BIT && num_channels == 1) {
      assert(sizeof(ItemType) == 2);
    }
    if (resolution == RESOLUTION_12_BIT && num_channels == 2) {
      assert(sizeof(ItemType) == 3);
    }
    if (resolution == RESOLUTION_12_BIT && num_channels == 1) {
      assert(sizeof(ItemType) == 3);
    }
    if (resolution == RESOLUTION_8_BIT && num_channels == 2) {
      assert(sizeof(ItemType) == 2);
    }
    if (resolution == RESOLUTION_8_BIT && num_channels == 1) {
      assert(sizeof(ItemType) == 1);
    }*/

    freeze_tail_ = static_cast<FloatFrame*>(static_cast<void*>(
        &buffers[num_banks - 1][bank_size - freeze_tail_size_in_bytes()]));
    for (size_t i = 0; i < num_banks; ++i) {
      items_[i] = static_cast<ItemType*>(static_cast<void*>(buffers[i]));
    }
    
    Reset();
  }
  
  inline size_t freeze_tail_size_in_bytes() const {
    return freeze_tail_size * sizeof(FloatFrame);
  }
  
  inline size_t delay_line_size_in_bytes() {
    return (max_delay + interpolation_tail) * sizeof(ItemType) / ItemType::bundle_size;
  }
  
  inline size_t items_per_bank() const {
    return bank_size / sizeof(ItemType);
  }
  
  inline uint8_t* raw_bytes(size_t offset) {
    return static_cast<uint8_t*>(
        static_cast<void*>(
            items_[offset / bank_size])) + (offset % bank_size);
  }
  
  inline int* mutable_write_ptr() { return &write_ptr_; }
  inline int write_ptr() const { return write_ptr_; }

  // Lich port: the state that Write() changes and the grains read.
  struct ReadState {
    int write_ptr;
    int duration;
    bool frozen;
  };
  inline ReadState read_state() const {
    ReadState s = { write_ptr_, duration_, frozen_ };
    return s;
  }
  inline void set_read_state(const ReadState& s) {
    write_ptr_ = s.write_ptr;
    duration_ = s.duration;
    frozen_ = s.frozen;
  }
  // The state after Write(in, frozen), without writing.
  inline void AdvanceReadState(ReadState* s, bool frozen) const {
    if (!frozen) {
      s->write_ptr = s->write_ptr == 0 ? max_delay - 1 : s->write_ptr - 1;
      s->duration = s->duration + 1 >= max_delay ? max_delay : s->duration + 1;
    }
    s->frozen = frozen;
  }
  
  inline void Reset(bool clear) {
    write_ptr_ = 0;
    tail_write_ptr_ = 0;
    tail_xfade_duration_ = 0;
    frozen_ = false;
    written_since_last_frozen_ = 0;
    duration_ = max_delay;
    
    if (clear) {
      FloatFrame zero;
      zero.l = zero.r = 0.0f;

      ItemType zero_item;
      zero_item.Store(0, zero);
      zero_item.Store(1, zero);
      for (size_t bank = 0; bank < num_banks; ++bank) {
        std::fill(&items_[bank][0], &items_[bank][items_per_bank()], zero_item);
      }
      std::fill(&freeze_tail_[0], &freeze_tail_[freeze_tail_size], zero);
    }
  }
  
  inline void Reset() {
    Reset(true);
  }
  
  inline void Write(FloatFrame in) {
    Store(write_ptr_, in);
    if (write_ptr_ < interpolation_tail) {
      Store(write_ptr_ + max_delay, in);
    }
    if (write_ptr_ == 0) {
      write_ptr_ = max_delay - 1;
    } else {
      --write_ptr_;
    }
    ++written_since_last_frozen_;
    if (written_since_last_frozen_ >= max_delay) {
      written_since_last_frozen_ = 0;
    }
    ++duration_;
    if (duration_ >= max_delay) {
      duration_ = max_delay;
    }
  }
  
  inline void Trim() {
    tail_write_ptr_ = 0;
    duration_ = 0;
  }
  
  inline void Write(FloatFrame in, bool frozen) {
    if (frozen) {
      if (!frozen_) {
        tail_write_ptr_ = 0;
      }
      if (tail_write_ptr_ < freeze_tail_size) {
        freeze_tail_[tail_write_ptr_].l = in.l;
        freeze_tail_[tail_write_ptr_].r = in.r;
        ++tail_write_ptr_;
      }
      written_since_last_frozen_ = 0;
    } else {
      if (frozen_ && tail_write_ptr_) {
        tail_xfade_duration_ = tail_write_ptr_;
      }
      if (tail_write_ptr_) {
        const int tail_position = tail_xfade_duration_ - tail_write_ptr_;
        const float t = float(tail_write_ptr_) / float(tail_xfade_duration_);
        const float gain = stmlib::SmoothStep(t);
        in.l += (freeze_tail_[tail_position].l - in.l) * gain;
        in.r += (freeze_tail_[tail_position].r - in.r) * gain;

        --tail_write_ptr_;
      }
      Write(in);
    }
    frozen_ = frozen;
  }
  
  template<Interpolation interpolation>
  inline FloatFrame Read(int32_t delay) const {
    int32_t delay_integral = delay >> 10;
    float delay_fractional = float(delay & 1023) / 1024.0f;
    return Read<interpolation>(delay_integral, delay_fractional);
  }
  
  template<Interpolation interpolation>
  inline FloatFrame Read(int delay_integral, float delay_fractional) const {
    return ReadAt<interpolation>(write_ptr_, delay_integral, delay_fractional);
  }

  // Lich port: read relative to a given write position, so that grains can
  // be rendered over a block before the block is written (see
  // GranularProcessor::RenderGrainsBlock).
  template<Interpolation interpolation>
  inline FloatFrame ReadAt(
      int write_ptr, int delay_integral, float delay_fractional) const {
    if (interpolation == INTERPOLATION_HERMITE) {
      delay_integral += max_delay - 1; // Fix latency.
    }
    int i = (write_ptr + delay_integral) % max_delay;
    
    FloatFrame value;
    
    if (interpolation == INTERPOLATION_ZOH) {
      value = Load(i);
    } else if (interpolation == INTERPOLATION_LINEAR) {
      const FloatFrame x0 = Load(i);
      const FloatFrame x1 = Load(i + 1);
      value = x0 + (x1 - x0) * delay_fractional;
    } else {
      FloatFrame x[4];
      LoadQuad(i, x);
      value = HermiteInterpolate(x, delay_fractional);
    }
    return value;
  }

  // Lich port: Read<INTERPOLATION_HERMITE>(position[s]) at write position
  // write_ptr[s], for the samples s of a block whose active[s] is set (see
  // ReadHermiteBlockOf).
  inline void ReadHermiteBlock(
      const int32_t* position,
      const int* write_ptr,
      const bool* active,
      FloatFrame* out,
      size_t size) const {
    ReadHermiteBlockOf(this, position, write_ptr, active, out, size);
  }

  // The same, for code that does not know the delay line's type. The frames
  // are read through a window of up to kHermiteWindowFrames consecutive
  // frames, copied from the buffer in one go (sequential reads, which the
  // Lich's SDRAM serves in bursts, instead of 4 scattered reads per sample).
  // Same values as ReadAt().
  static void ReadHermiteBlockOf(
      const void* delay_line,
      const int32_t* position,
      const int* write_ptr,
      const bool* active,
      FloatFrame* out,
      size_t size) {
    HermiteBlock(&LoadWindowOf, delay_line, max_delay, position, write_ptr,
                 active, out, size);
  }

  // Frames first to first + count - 1 (count <= kHermiteWindowFrames).
  static void LoadWindowOf(
      const void* delay_line, int first, int count, FloatFrame* frames) {
    const DelayLine* d = static_cast<const DelayLine*>(delay_line);
    const int bundle = ItemType::bundle_size;
    const int first_item = first / bundle;
    const int last_item = (first + count - 1) / bundle;
    ItemType items[kHermiteWindowFrames + 1];
    for (int n = first_item; n <= last_item; ) {
      const size_t offset = size_t(n) % d->items_per_bank();
      const size_t bank = size_t(n) / d->items_per_bank();
      const int items_count = std::min(
          last_item - n + 1, int(d->items_per_bank() - offset));
      memcpy(&items[n - first_item], &d->items_[bank][offset],
             items_count * sizeof(ItemType));
      n += items_count;
    }
    for (int f = first; f < first + count; ++f) {
      frames[f - first] = items[f / bundle - first_item].value(f % bundle);
    }
  }
  
  template<Interpolation interpolation>
  inline FloatFrame Read(float delay) const {
    MAKE_INTEGRAL_FRACTIONAL(delay)
    return Read<interpolation>(delay_integral, delay_fractional);
  }
  
  inline bool frozen() const { return frozen_; }
  
  inline int duration() const { return duration_; }

  inline int remaining() const {
    return max_delay - written_since_last_frozen_;
  }
  
 private:
  inline void Store(int i, FloatFrame value) {
    mutable_item(i / ItemType::bundle_size)->Store(
        i % ItemType::bundle_size, value);
  }
  
  inline FloatFrame Load(int i) const {
    return item(i / ItemType::bundle_size).value(i % ItemType::bundle_size);
  }
  
  inline void LoadQuad(int i, FloatFrame* x) const {
    // Try to find optimizations if there are several banks and if the
    // memory layout is not too gnarly.
    if (num_banks != 1) {
      if (ItemType::bundle_size == 1) {
        // Load the first item.
        size_t offset = size_t(i) % items_per_bank();
        size_t bank = size_t(i) / items_per_bank();
        if ((offset + 3) < items_per_bank()) {
          // Optimization found! All items belong in the same bank!
          x[0] = items_[bank][offset].value(0);
          x[1] = items_[bank][offset + 1].value(0);
          x[2] = items_[bank][offset + 2].value(0);
          x[3] = items_[bank][offset + 3].value(0);
          return;
        }
      } else if (ItemType::bundle_size == 2) {
        // Load the first item.
        size_t offset = size_t(i >> 1) % items_per_bank();
        size_t bank = size_t(i >> 1) / items_per_bank();

        if ((offset + 2) < items_per_bank()) {
          // Optimization found! All items belong in the same bank!
          if ((i % 2) == 0) {
            x[0] = items_[bank][offset].value(0);
            x[1] = items_[bank][offset].value(1);
            x[2] = items_[bank][offset + 1].value(0);
            x[3] = items_[bank][offset + 1].value(1);
          } else {
            x[0] = items_[bank][offset].value(1);
            x[1] = items_[bank][offset + 1].value(0);
            x[2] = items_[bank][offset + 1].value(1);
            x[3] = items_[bank][offset + 2].value(0);
          }
          return;
        }
      }
    }
    
    // No optmization was found, use the slow code.
    x[0] = Load(i);
    x[1] = Load(i + 1);
    x[2] = Load(i + 2);
    x[3] = Load(i + 3);
  }
   
  // Non-const flavor write access.
  inline ItemType* mutable_item(int i) {
    return const_cast<ItemType*>(&item(i));
  }
  
  inline const ItemType& item(int i) const {
    if (num_banks == 1) {
      return items_[0][i];
    } else {
      size_t offset = size_t(i) % items_per_bank();
      size_t bank = size_t(i) / items_per_bank();
      return items_[bank][offset];
    }
  }
  
  ItemType* items_[num_banks];
  FloatFrame* freeze_tail_;
  
  int write_ptr_;
  int tail_write_ptr_;
  int tail_xfade_duration_;
  int written_since_last_frozen_;
  int duration_;
  
  bool frozen_;

  DISALLOW_COPY_AND_ASSIGN(DelayLine);
};

}  // namespace beads

#endif  // BEADS_DSP_DELAY_LINE_H_
