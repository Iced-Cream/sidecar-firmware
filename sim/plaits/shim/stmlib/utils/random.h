// Copyright 2012 Emilie Gillet.
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
// Fast 16-bit pseudo random number generator.
// rytm-teensy: stmlib's random.h with its state per render core when RYTM_RENDER_CORES > 1 (the Pi engine renders
// voices on several cores at once: one shared state raced, and a core's noise depended on the other core's timing).
// rytm_render_core() picks the core, as sim/plaits/shim/plaits/user_data.h. Unset (the Teensy, the simulator):
// stmlib's own single state, unchanged. teensy/setup_plaits_lib.sh copies this over the vendor's; the simulator's
// build has shim/ first on the include path.

#ifndef STMLIB_UTILS_RANDOM_H_
#define STMLIB_UTILS_RANDOM_H_

#include "stmlib/stmlib.h"

#ifndef RYTM_RENDER_CORES
#define RYTM_RENDER_CORES 1
#endif  // RYTM_RENDER_CORES
#if RYTM_RENDER_CORES > 1
extern "C" int rytm_render_core();
#endif  // RYTM_RENDER_CORES > 1

namespace stmlib {

class Random {
 public:
  static inline uint32_t state() { return rng_state(); }

  static inline void Seed(uint32_t seed) {
#if RYTM_RENDER_CORES > 1
    for (int c = 1; c < RYTM_RENDER_CORES; ++c) more_states()[c - 1] = seed;   // every core: one sequence each
#endif  // RYTM_RENDER_CORES > 1
    rng_state_ = seed;
  }

  static inline uint32_t GetWord() {
    uint32_t& s = rng_state();
    s = s * 1664525L + 1013904223L;
    return s;
  }
  
  static inline int16_t GetSample() {
    return static_cast<int16_t>(GetWord() >> 16);
  }

  static inline float GetFloat() {
    return static_cast<float>(GetWord()) / 4294967296.0f;
  }

 private:
  static uint32_t rng_state_;   // core 0's (random.cc: 0x21)

#if RYTM_RENDER_CORES > 1
  static inline uint32_t* more_states() {   // cores 1.. , starting as core 0 does
    static_assert(RYTM_RENDER_CORES <= 9, "more_states holds 8");
    static uint32_t s[8] = {0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21};
    return s;
  }
  static inline uint32_t& rng_state() {
    int c = rytm_render_core();
    return c ? more_states()[c - 1] : rng_state_;
  }
#else
  static inline uint32_t& rng_state() { return rng_state_; }
#endif  // RYTM_RENDER_CORES > 1

  DISALLOW_COPY_AND_ASSIGN(Random);
};

}  // namespace stmlib

#endif  // STMLIB_UTILS_RANDOM_H_
