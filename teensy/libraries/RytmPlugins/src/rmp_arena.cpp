// rmp_arena.cpp
#include "rmp_arena.h"
#include "rmp_arenas.h"
#include <Arduino.h>
#ifndef SCB_ICIALLU
#define SCB_ICIALLU (*(volatile uint32_t *)0xE000EF50)
#endif
// 8 KB aligned (an MPU subregion), in its own section: teensy/cold_ld.sh puts .dtcm_arena first in
// DTCM (0x20000000, the first input of .data), so no padding before it is lost to the stack (in .bss
// it followed .data up to the next 8 KB step).
// The MPU covers it with 64 KB regions whose 8 KB subregions are enabled only over
// [arena, arena + RMP_ARENA_BYTES): one window, or two when it straddles a 64 KB line.
// extern "C" and not static: the symbol is rmp_arena_mem, unmangled, for run_sketch.sh's nm check.
extern "C" {
__attribute__((section(".dtcm_arena"), aligned(RMP_ARENA_ALIGN))) uint8_t rmp_arena_mem[RMP_ARENA_BYTES];
}
static uint32_t ctrl_before_, ctrl_after_;
// [RMP_PLACE_INTERNAL] the DTCM arena above; [RMP_PLACE_PSRAM] the PSRAM one (size 0 without PSRAM)
static rmp_arenas ar = {{{rmp_arena_mem, RMP_ARENA_BYTES, 0}, {nullptr, 0, 0}}};
static_assert(RMP_ARENA_BYTES % RMP_ARENA_ALIGN == 0 && RMP_ARENA_BYTES <= 65536u,
              "the arena is whole 8 KB subregions, at most two 64 KB windows");

// SRD for window w (64 KB at w): bit k set = subregion k (8 KB at w + 8K k) disabled.
static uint32_t srd_for(uint32_t w, uint32_t a, uint32_t e) {
  uint32_t srd = 0;
  for (uint32_t k = 0; k < 8; ++k) {
    uint32_t lo = w + k * 8192u, hi = lo + 8192u;
    if (hi <= a || lo >= e) srd |= 1u << k;
  }
  return srd;
}

FLASHMEM void rmp_arena_begin() {
  // The measured sequence (teensy/arena_spike): the core's configure_cache() ends with
  // SCB_MPU_CTRL = SCB_MPU_CTRL_ENABLE (no PRIVDEFENA); keep whatever it set.
  const uint32_t a = (uint32_t)rmp_arena_mem, e = a + RMP_ARENA_BYTES;
  const uint32_t w0 = a & ~0xFFFFu, w1 = w0 + 0x10000u;
  const uint32_t srd0 = srd_for(w0, a, e), srd1 = srd_for(w1, a, e);
  // normal memory, write-back cacheable (DTCM is not cached anyway), full access, executable (no XN), 64 KB
  const uint32_t attr = SCB_MPU_RASR_TEX(1) | SCB_MPU_RASR_C | SCB_MPU_RASR_B | SCB_MPU_RASR_AP(3) |
                        SCB_MPU_RASR_SIZE(15) | SCB_MPU_RASR_ENABLE;
  uint32_t pm; asm volatile("mrs %0, primask; cpsid i" : "=r"(pm) :: "memory");
  ctrl_before_ = SCB_MPU_CTRL;
  SCB_MPU_CTRL = 0;
  SCB_MPU_RBAR = w0 | SCB_MPU_RBAR_REGION(RMP_ARENA_REGION) | SCB_MPU_RBAR_VALID;
  SCB_MPU_RASR = attr | (srd0 << 8);
  SCB_MPU_RBAR = w1 | SCB_MPU_RBAR_REGION(RMP_ARENA_REGION2) | SCB_MPU_RBAR_VALID;
  SCB_MPU_RASR = srd1 == 0xFFu ? 0 : attr | (srd1 << 8);   // not needed: region off
  SCB_MPU_CTRL = ctrl_before_;
  asm volatile("dsb; isb" ::: "memory");
  ctrl_after_ = SCB_MPU_CTRL;
  asm volatile("msr primask, %0" :: "r"(pm) : "memory");
  ar.a[RMP_PLACE_INTERNAL].used = 0;
}
FLASHMEM void rmp_arena_mpu_readback(uint32_t *cb, uint32_t *ca, uint32_t *rbar, uint32_t *rasr,
                                     uint32_t *rbar2, uint32_t *rasr2) {
  uint32_t pm; asm volatile("mrs %0, primask; cpsid i" : "=r"(pm) :: "memory");
  uint32_t ctrl = SCB_MPU_CTRL;
  SCB_MPU_CTRL = 0;
  SCB_MPU_RNR = RMP_ARENA_REGION;
  *rbar = SCB_MPU_RBAR; *rasr = SCB_MPU_RASR;
  SCB_MPU_RNR = RMP_ARENA_REGION2;
  *rbar2 = SCB_MPU_RBAR; *rasr2 = SCB_MPU_RASR;
  SCB_MPU_CTRL = ctrl;
  asm volatile("dsb; isb" ::: "memory");
  asm volatile("msr primask, %0" :: "r"(pm) : "memory");
  *cb = ctrl_before_; *ca = ctrl_after_;
}
uint8_t *rmp_arena_base() { return rmp_arena_mem; }
// Internal first, then PSRAM (when there is a PSRAM arena): rmp_arenas.h.
uint8_t *rmp_arena_alloc(uint32_t bytes) { return rmp_arenas_alloc(&ar, bytes); }
FLASHMEM uint8_t *rmp_arena_alloc_in(int where, uint32_t bytes) { return rmp_arenas_alloc_in(&ar, where, bytes); }
void rmp_arena_reset() { rmp_arenas_reset(&ar); }
uint32_t rmp_arena_used() { return ar.a[RMP_PLACE_INTERNAL].used; }
FLASHMEM uint32_t rmp_arena_psram_size() { return ar.a[RMP_PLACE_PSRAM].size; }
FLASHMEM uint32_t rmp_arena_psram_used() { return ar.a[RMP_PLACE_PSRAM].used; }
FLASHMEM uint8_t *rmp_arena_psram_base() { return ar.a[RMP_PLACE_PSRAM].base; }
FLASHMEM int rmp_arena_where(const void *p) { return rmp_arenas_where(&ar, p); }
FLASHMEM void rmp_arena_mark(uint32_t *u_int, uint32_t *u_ps) { *u_int = ar.a[0].used; *u_ps = ar.a[1].used; }
FLASHMEM void rmp_arena_release(uint32_t u_int, uint32_t u_ps) {
  if (u_int <= ar.a[0].used) ar.a[0].used = u_int;
  if (u_ps <= ar.a[1].used) ar.a[1].used = u_ps;
}

// ---- the PSRAM arena --------------------------------------------------------------------------------------------
// One block from extmem_malloc (the core's PSRAM heap), aligned inside it to an MPU subregion (R / 8), R the
// arena's size (a power of two, 64 KB..8 MB). Regions RMP_PSRAM_REGION / _REGION2 (R each, R-aligned windows)
// enable only the subregions over [a, a + R): normal memory, write-back write-allocate cacheable (as the core's
// own FlexSPI2 region 9), full access, executable - region 9 (0x7000_0000, 32 MB) is XN, and the higher number
// wins over it. Without PSRAM nothing here runs and the MPU is not touched.
extern "C" uint8_t external_psram_size;   // MB, found by the core at boot (startup.c)
static uint8_t *ps_raw;
static uint32_t ps_rbar[2], ps_rasr[2];
FLASHMEM uint32_t rmp_arena_psram_begin(uint32_t bytes) {
  if (ps_raw || !external_psram_size || !bytes) return ar.a[RMP_PLACE_PSRAM].size;
  uint32_t r = 65536u;
  while (r < bytes && r < (8u << 20)) r <<= 1;
  const uint32_t sub = r / 8u;
  uint8_t *m = (uint8_t *)extmem_malloc(r + sub);
  // extmem_malloc falls back to the heap when PSRAM cannot take it: only 0x7000_0000.. will do
  if (!m || ((uint32_t)m >> 28) != 7) { if (m) extmem_free(m); return 0; }
  const uint32_t a = ((uint32_t)m + sub - 1u) & ~(sub - 1u), e = a + r;
  const uint32_t w0 = a & ~(r - 1u), w1 = w0 + r;
  const uint32_t srd0 = rmp_mpu_srd(w0, sub, a, e), srd1 = rmp_mpu_srd(w1, sub, a, e);
  uint32_t lg = 0;
  while ((1u << lg) < r) ++lg;
  const uint32_t attr = SCB_MPU_RASR_TEX(1) | SCB_MPU_RASR_C | SCB_MPU_RASR_B | SCB_MPU_RASR_AP(3) |
                        SCB_MPU_RASR_SIZE(lg - 1u) | SCB_MPU_RASR_ENABLE;   // SIZE n: 2^(n+1) bytes; no XN
  ps_rbar[0] = w0 | SCB_MPU_RBAR_REGION(RMP_PSRAM_REGION) | SCB_MPU_RBAR_VALID;
  ps_rasr[0] = srd0 == 0xFFu ? 0 : attr | (srd0 << 8);
  ps_rbar[1] = w1 | SCB_MPU_RBAR_REGION(RMP_PSRAM_REGION2) | SCB_MPU_RBAR_VALID;
  ps_rasr[1] = srd1 == 0xFFu ? 0 : attr | (srd1 << 8);
  memset((void *)a, 0, r);
  arm_dcache_flush_delete((void *)a, r);   // the zeroes in PSRAM, no line of it left in the D-cache
  uint32_t pm; asm volatile("mrs %0, primask; cpsid i" : "=r"(pm) :: "memory");
  uint32_t ctrl = SCB_MPU_CTRL;
  SCB_MPU_CTRL = 0;
  SCB_MPU_RBAR = ps_rbar[0]; SCB_MPU_RASR = ps_rasr[0];
  SCB_MPU_RBAR = ps_rbar[1]; SCB_MPU_RASR = ps_rasr[1];
  SCB_MPU_CTRL = ctrl;
  asm volatile("dsb; isb" ::: "memory");
  asm volatile("msr primask, %0" :: "r"(pm) : "memory");
  SCB_ICIALLU = 0;
  asm volatile("dsb; isb" ::: "memory");
  ps_raw = m;
  ar.a[RMP_PLACE_PSRAM] = rmp_bump{(uint8_t *)a, r, 0};
  return r;
}
FLASHMEM void rmp_arena_psram_mpu(uint32_t *rbar, uint32_t *rasr, uint32_t *rbar2, uint32_t *rasr2) {
  uint32_t pm; asm volatile("mrs %0, primask; cpsid i" : "=r"(pm) :: "memory");
  uint32_t ctrl = SCB_MPU_CTRL;
  SCB_MPU_CTRL = 0;
  SCB_MPU_RNR = RMP_PSRAM_REGION;
  *rbar = SCB_MPU_RBAR; *rasr = SCB_MPU_RASR;
  SCB_MPU_RNR = RMP_PSRAM_REGION2;
  *rbar2 = SCB_MPU_RBAR; *rasr2 = SCB_MPU_RASR;
  SCB_MPU_CTRL = ctrl;
  asm volatile("dsb; isb" ::: "memory");
  asm volatile("msr primask, %0" :: "r"(pm) : "memory");
}
// The same for both arenas: DTCM is not cached (the clean is a no-op there); in PSRAM the clean writes the
// code out over FlexSPI2 and the I-cache invalidate drops any line of an older image at these addresses.
void rmp_arena_sync(void *p, uint32_t bytes) {
  arm_dcache_flush(p, bytes);
  asm volatile("dsb; isb" ::: "memory");
  SCB_ICIALLU = 0;
  asm volatile("dsb; isb" ::: "memory");
}
