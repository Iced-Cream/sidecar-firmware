// core/host/newlib_rand.cpp - newlib's rand()/srand() on the Mac (the Teensy's and the Pi's libc), so DAISYSP's
// exciter noise follows the Teensy's sequence in the golden comparisons. Inside libcore.dylib and the test
// programs only: their own calls bind to these definitions. newlib: next = next * 6364136223846793005 + 1,
// result (next >> 32) & 0x7fffffff, the state starting at 1.
#include <stdint.h>
#include <stdlib.h>

// RYTM_RENDER_CORES > 1 (the Pi: cores 1 and 2 render at once): a state per core, rytm_render_core() the calling one
// (as sim/plaits/shim/plaits/user_data.h), so a core's sequence does not depend on the other's timing; srand seeds all.
#ifndef RYTM_RENDER_CORES
#define RYTM_RENDER_CORES 1
#endif
#if RYTM_RENDER_CORES > 1
extern "C" int rytm_render_core();
static uint64_t g_next[8] = {1, 1, 1, 1, 1, 1, 1, 1};   // cores 0..RYTM_RENDER_CORES-1 used
static_assert(RYTM_RENDER_CORES <= 8, "g_next holds 8");
static inline uint64_t& next() { return g_next[rytm_render_core()]; }
#else
static uint64_t g_next[1] = {1};
static inline uint64_t& next() { return g_next[0]; }
#endif

extern "C" int rand(void) {
  uint64_t& n = next();
  n = n * 6364136223846793005ull + 1;
  return (int)((n >> 32) & 0x7fffffff);
}

extern "C" void srand(unsigned seed) {
  for (uint64_t& n : g_next) n = seed;
}
