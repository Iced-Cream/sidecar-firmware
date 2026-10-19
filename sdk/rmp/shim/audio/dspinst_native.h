/* Derived from the Audio library's utility/dspinst.h (Teensyduino 1.62); PJRC's notice follows, as the licence requires. */
/* Audio Library for Teensy 3.X
 * Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards.  Please support PJRC's efforts to develop
 * open source software by purchasing Teensy or other PJRC products.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/* sdk/rmp/shim/audio/dspinst_native.h - portable C for the Audio library's
   utility/dspinst.h (Cortex-M7 DSP instructions), for the native (Mac) build only.
   The Arduino.h shim includes it and defines __ARM_ARCH_7EM__ off the Teensy, so the
   Audio objects compile their Teensy 4 code paths with these functions in place of
   the inline assembly; the include guard keeps the library's own dspinst.h out.
   Each function computes what the instruction named in its comment computes. */
#ifndef dspinst_h_
#define dspinst_h_
#include <stdint.h>

static inline int32_t dsp_sat(int64_t v, int bits) {
  int64_t m = (int64_t)1 << (bits - 1);
  return (int32_t)(v > m - 1 ? m - 1 : v < -m ? -m : v);
}
static inline int16_t dsp_hi(uint32_t x) { return (int16_t)(x >> 16); }
static inline int16_t dsp_lo(uint32_t x) { return (int16_t)(x & 0xFFFF); }
static inline uint32_t dsp_pack(int32_t hi, int32_t lo) { return ((uint32_t)(uint16_t)hi << 16) | (uint16_t)lo; }

/* ssat #bits, val, asr #rshift */
static inline int32_t signed_saturate_rshift(int32_t val, int bits, int rshift) { return dsp_sat(val >> rshift, bits); }
static inline int16_t saturate16(int32_t val) { return (int16_t)dsp_sat(val, 16); }
/* smulwb / smulwt */
static inline int32_t signed_multiply_32x16b(int32_t a, uint32_t b) { return (int32_t)(((int64_t)a * dsp_lo(b)) >> 16); }
static inline int32_t signed_multiply_32x16t(int32_t a, uint32_t b) { return (int32_t)(((int64_t)a * dsp_hi(b)) >> 16); }
/* smmul / smmulr / smmlar / smmlsr */
static inline int32_t multiply_32x32_rshift32(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 32); }
static inline int32_t multiply_32x32_rshift32_rounded(int32_t a, int32_t b) {
  return (int32_t)(uint32_t)(((uint64_t)((int64_t)a * b) + 0x80000000u) >> 32);
}
/* sum * 2^32 +- a * b + 2^31, top word: in uint64_t (wraps as the instruction does, no signed overflow) */
static inline int32_t multiply_accumulate_32x32_rshift32_rounded(int32_t sum, int32_t a, int32_t b) {
  return (int32_t)(uint32_t)((((uint64_t)(uint32_t)sum << 32) + (uint64_t)((int64_t)a * b) + 0x80000000u) >> 32);
}
static inline int32_t multiply_subtract_32x32_rshift32_rounded(int32_t sum, int32_t a, int32_t b) {
  return (int32_t)(uint32_t)((((uint64_t)(uint32_t)sum << 32) - (uint64_t)((int64_t)a * b) + 0x80000000u) >> 32);
}
/* pkhtb asr #16 / pkhtb / pkhbt lsl #16 */
static inline uint32_t pack_16t_16t(int32_t a, int32_t b) { return ((uint32_t)a & 0xFFFF0000u) | ((uint32_t)b >> 16); }
static inline uint32_t pack_16t_16b(int32_t a, int32_t b) { return ((uint32_t)a & 0xFFFF0000u) | ((uint32_t)b & 0xFFFFu); }
static inline uint32_t pack_16b_16b(int32_t a, int32_t b) { return ((uint32_t)a << 16) | ((uint32_t)b & 0xFFFFu); }
/* qadd16 / qsub16 / shadd16 / shsub16 */
static inline uint32_t signed_add_16_and_16(uint32_t a, uint32_t b) {
  return dsp_pack(dsp_sat(dsp_hi(a) + dsp_hi(b), 16), dsp_sat(dsp_lo(a) + dsp_lo(b), 16));
}
static inline int32_t signed_subtract_16_and_16(int32_t a, int32_t b) {
  return (int32_t)dsp_pack(dsp_sat(dsp_hi(a) - dsp_hi(b), 16), dsp_sat(dsp_lo(a) - dsp_lo(b), 16));
}
static inline int32_t signed_halving_add_16_and_16(int32_t a, int32_t b) {
  return (int32_t)dsp_pack((dsp_hi(a) + dsp_hi(b)) >> 1, (dsp_lo(a) + dsp_lo(b)) >> 1);
}
static inline int32_t signed_halving_subtract_16_and_16(int32_t a, int32_t b) {
  return (int32_t)dsp_pack((dsp_hi(a) - dsp_hi(b)) >> 1, (dsp_lo(a) - dsp_lo(b)) >> 1);
}
/* smlawb / smlawt */
static inline int32_t signed_multiply_accumulate_32x16b(int32_t sum, int32_t a, uint32_t b) {
  return (int32_t)((uint32_t)sum + (uint32_t)(int32_t)(((int64_t)a * dsp_lo(b)) >> 16));
}
static inline int32_t signed_multiply_accumulate_32x16t(int32_t sum, int32_t a, uint32_t b) {
  return (int32_t)((uint32_t)sum + (uint32_t)(int32_t)(((int64_t)a * dsp_hi(b)) >> 16));
}
static inline uint32_t logical_and(uint32_t a, uint32_t b) { return a & b; }
/* smuad / smuadx / smlald / smlaldx */
static inline int32_t multiply_16tx16t_add_16bx16b(uint32_t a, uint32_t b) {
  return (int32_t)((uint32_t)(dsp_lo(a) * dsp_lo(b)) + (uint32_t)(dsp_hi(a) * dsp_hi(b)));
}
static inline int32_t multiply_16tx16b_add_16bx16t(uint32_t a, uint32_t b) {
  return (int32_t)((uint32_t)(dsp_lo(a) * dsp_hi(b)) + (uint32_t)(dsp_hi(a) * dsp_lo(b)));
}
static inline int64_t multiply_accumulate_16tx16t_add_16bx16b(int64_t sum, uint32_t a, uint32_t b) {
  return sum + (int64_t)dsp_lo(a) * dsp_lo(b) + (int64_t)dsp_hi(a) * dsp_hi(b);
}
static inline int64_t multiply_accumulate_16tx16b_add_16bx16t(int64_t sum, uint32_t a, uint32_t b) {
  return sum + (int64_t)dsp_lo(a) * dsp_hi(b) + (int64_t)dsp_hi(a) * dsp_lo(b);
}
/* smulbb / smulbt / smultb / smultt */
static inline int32_t multiply_16bx16b(uint32_t a, uint32_t b) { return dsp_lo(a) * dsp_lo(b); }
static inline int32_t multiply_16bx16t(uint32_t a, uint32_t b) { return dsp_lo(a) * dsp_hi(b); }
static inline int32_t multiply_16tx16b(uint32_t a, uint32_t b) { return dsp_hi(a) * dsp_lo(b); }
static inline int32_t multiply_16tx16t(uint32_t a, uint32_t b) { return dsp_hi(a) * dsp_hi(b); }
/* qsub */
static inline int32_t substract_32_saturate(uint32_t a, uint32_t b) { return dsp_sat((int64_t)(int32_t)a - (int32_t)b, 32); }
/* smull; asl; orr lsr: ((int64_t)x * y) << (z + 1) >> 32 */
static inline int32_t FRACMUL_SHL(int32_t x, int32_t y, int z) { return (int32_t)(((int64_t)x * y) >> (31 - z)); }
/* the Q flag: no sticky saturation flag off the Teensy */
static inline uint32_t get_q_psr(void) { return 0; }
static inline void clr_q_psr(void) {}
#endif
