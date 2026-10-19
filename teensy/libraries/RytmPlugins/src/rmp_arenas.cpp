// rmp_arenas.cpp - see rmp_arenas.h. Cold (flash on the Teensy): loads and reports only.
#include "rmp_arenas.h"
#include "rmp_loader.h"   // RMP_COLD
#if defined(__IMXRT1062__)   // the names in program flash (a plain literal would take RAM1)
static const char n_int[] __attribute__((section(".progmem"))) = "internal";
static const char n_ps[] __attribute__((section(".progmem"))) = "psram";
static const char n_none[] __attribute__((section(".progmem"))) = "-";
#else
static const char n_int[] = "internal", n_ps[] = "psram", n_none[] = "-";
#endif

RMP_COLD uint8_t *rmp_bump_alloc(rmp_bump *b, uint32_t bytes) {
  if (!b->base || !b->size) return nullptr;
  uint32_t a = (b->used + 7u) & ~7u;
  if (a > b->size || bytes > b->size - a) return nullptr;
  b->used = a + bytes;
  return b->base + a;
}
RMP_COLD uint8_t *rmp_arenas_alloc(rmp_arenas *r, uint32_t bytes) {
  uint8_t *p = rmp_bump_alloc(&r->a[RMP_PLACE_INTERNAL], bytes);
  return p ? p : rmp_bump_alloc(&r->a[RMP_PLACE_PSRAM], bytes);
}
RMP_COLD uint8_t *rmp_arenas_alloc_in(rmp_arenas *r, int where, uint32_t bytes) {
  return where == RMP_PLACE_INTERNAL || where == RMP_PLACE_PSRAM ? rmp_bump_alloc(&r->a[where], bytes) : nullptr;
}
RMP_COLD void rmp_arenas_reset(rmp_arenas *r) { r->a[0].used = 0; r->a[1].used = 0; }
RMP_COLD int rmp_arenas_where(const rmp_arenas *r, const void *p) {
  const uint8_t *q = (const uint8_t *)p;
  for (int k = 0; k < 2; ++k)
    if (r->a[k].base && r->a[k].size && q >= r->a[k].base && q < r->a[k].base + r->a[k].size) return k;
  return RMP_PLACE_NONE;
}
RMP_COLD const char *rmp_place_name(int where) {
  return where == RMP_PLACE_INTERNAL ? n_int : where == RMP_PLACE_PSRAM ? n_ps : n_none;
}
RMP_COLD uint32_t rmp_mpu_srd(uint32_t w, uint32_t sub, uint32_t a, uint32_t e) {
  uint32_t srd = 0;
  for (uint32_t k = 0; k < 8; ++k) {
    uint64_t lo = (uint64_t)w + (uint64_t)k * sub, hi = lo + sub;
    if (hi <= a || lo >= e) srd |= 1u << k;
  }
  return srd;
}
