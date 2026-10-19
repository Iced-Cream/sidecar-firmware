/* sdk/rmp/shim/audio/Arduino.h - the few Arduino definitions the Teensy Audio
   Library objects (and Noise Plethora's plugins) use, for a plugin image: no core,
   no interrupts, no heap. Valid C and C++ (the library's data_*.c tables are C).
   random() runs on the current AudioGraph's xorshift state (AudioStream.h), so each
   voice has its own sequence. Off the Teensy (the native build) it also selects
   the Audio objects' Teensy 4 code paths with portable dspinst functions. */
#ifndef RMP_ARDUINO_SHIM_H
#define RMP_ARDUINO_SHIM_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#if !defined(__arm__)
#ifndef __ARM_ARCH_7EM__
#define __ARM_ARCH_7EM__ 1
#endif
#include "dspinst_native.h"
#endif

#define PROGMEM
#define FLASHMEM
#define DMAMEM
#define FASTRUN
#define F_CPU_ACTUAL 600000000u
#define F_CPU 600000000u
#ifndef __disable_irq
#define __disable_irq() do {} while (0)
#define __enable_irq() do {} while (0)
#endif

#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105

#ifdef __cplusplus
extern "C" {
#endif
uint32_t audio_graph_random(void);   /* AudioStream.cpp: xorshift32 of the current graph */
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
/* as the Teensy core: random(howbig) 0..howbig-1, random(min, max) min..max-1 */
static inline uint32_t random(uint32_t howbig) { return howbig ? audio_graph_random() % howbig : 0; }
static inline int32_t random(int32_t howsmall, int32_t howbig) {
  if (howsmall >= howbig) return howsmall;
  return howsmall + (int32_t)random((uint32_t)(howbig - howsmall));
}
template <class A, class B> static inline auto min(A a, B b) -> decltype(a < b ? a : b) { return a < b ? a : b; }
template <class A, class B> static inline auto max(A a, B b) -> decltype(a > b ? a : b) { return a > b ? a : b; }
template <class T, class L, class H> static inline T constrain(T x, L lo, H hi) { return x < lo ? lo : x > hi ? hi : x; }
#else
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#endif
#endif
