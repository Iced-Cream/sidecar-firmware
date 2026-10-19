// rmp_meta.cpp - see rmp_meta.h.
#include "rmp_meta.h"
#include "rmp_proto.h"
#include <string.h>

static uint8_t next_gen(uint8_t g) { return g >= 127 ? 1 : (uint8_t)(g + 1); }
RMP_COLD static uint32_t fnv(uint32_t h, const void *p, uint32_t n) {
  const uint8_t *b = (const uint8_t *)p;
  while (n--) { h ^= *b++; h *= 16777619u; }
  return h;
}

// seed 1: 1; else 1..127 from the seed's mix with field k (0..4 the slots, 5 the store)
RMP_COLD static uint8_t seed_gen(uint32_t seed, uint32_t k) {
  if (seed == 1) return 1;
  uint32_t x = seed ^ (k + 1) * 0x9E3779B9u;
  x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13; x *= 0xC2B2AE35u; x ^= x >> 16;
  return (uint8_t)(1 + x % 127);
}

RMP_COLD void rmp_meta_init(rmp_meta *m, uint32_t seed) {
  uint32_t empty = rmp_meta_print(RMP_PST_EMPTY, 0, "", 0, nullptr);
  for (int s = 0; s < RMP_META_SLOTS; ++s) { m->gen[s] = seed_gen(seed, (uint32_t)s); m->print[s] = empty; }
  m->store_gen = seed_gen(seed, RMP_META_SLOTS);
  m->dirty = true;
}

RMP_COLD uint32_t rmp_meta_print(int pstate, int flags, const char *id, uint32_t version, const rmp_desc *d) {
  uint32_t h = 2166136261u;
  uint8_t sf[2] = {(uint8_t)pstate, (uint8_t)flags};
  h = fnv(h, sf, 2);
  if (!id) id = "";
  h = fnv(h, id, (uint32_t)strnlen(id, 32));
  h = fnv(h, &version, 4);
  if (d) {
    h = fnv(h, d->name, sizeof d->name);
    h = fnv(h, d->short_name, sizeof d->short_name);
    h = fnv(h, d->p2_title, sizeof d->p2_title);
    h = fnv(h, &d->page2, 1);
    uint8_t nk = d->nknobs > RMP_DESC_KNOBS ? (uint8_t)RMP_DESC_KNOBS : d->nknobs;
    h = fnv(h, &nk, 1);
    h = fnv(h, d->knob, (uint32_t)(nk * sizeof d->knob[0]));
  }
  return h;
}

RMP_COLD bool rmp_meta_set(rmp_meta *m, int s, uint32_t print) {
  if (s < 0 || s >= RMP_META_SLOTS || m->print[s] == print) return false;
  m->print[s] = print;
  m->gen[s] = next_gen(m->gen[s]);
  m->dirty = true;
  return true;
}

RMP_COLD void rmp_meta_bump(rmp_meta *m, int s) {
  if (s < 0 || s >= RMP_META_SLOTS) return;
  m->gen[s] = next_gen(m->gen[s]);
  m->print[s] = m->print[s] * 2654435761u + 1u;   // no print matches it, after any number of bumps (not ~, which two
                                                  // bumps undo): the settled slot's next rmp_meta_set always bumps (a push)
  m->dirty = true;
}

RMP_COLD void rmp_meta_store(rmp_meta *m) {
  m->store_gen = next_gen(m->store_gen);
  m->dirty = true;
}

RMP_COLD bool rmp_meta_take(rmp_meta *m) {
  bool d = m->dirty;
  m->dirty = false;
  return d;
}
