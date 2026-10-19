// extras/arenas_test.cpp - the two code arenas (internal + PSRAM): placement, NO_ROOM, reset, MPU windows,
// limits, and rmp_slots_load over two simulated arenas (with and without the PSRAM one), on the Mac.
// build: c++ -std=c++17 -O1 -Wall -DNDEBUG -I../src arenas_test.cpp ../src/rmp_arenas.cpp ../src/rmp_slots.cpp ../src/rmp_loader.cpp -o /tmp/arenas_test && /tmp/arenas_test
// (run from extras/; argv[1] overrides the path of sine_test.rmp)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "rmp_arenas.h"
#include "rmp_slots.h"
#include "rmp_api.h"

static std::vector<uint8_t> read_file(const char *p) {
  FILE *f = fopen(p, "rb"); CHECK(f);
  std::vector<uint8_t> v; uint8_t b[4096]; size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
  fclose(f); return v;
}
static void set_id(std::vector<uint8_t> &p, const char *id) {   // another package id, CRC unchanged (the header is not CRC'd)
  rmp_header h; memcpy(&h, p.data(), sizeof h); memset(h.id, 0, sizeof h.id); strcpy(h.id, id); memcpy(p.data(), &h, sizeof h);
}

// ---- the fake platform: an in-memory store and two arenas ----
static std::map<std::string, std::vector<uint8_t>> store;
static bool f_size_of(const char *id, uint32_t *len) {
  auto it = store.find(id); if (it == store.end()) return false;
  *len = (uint32_t)it->second.size(); return true;
}
static bool f_read_at(const char *id, uint32_t off, uint8_t *buf, uint32_t n) {
  auto it = store.find(id); if (it == store.end()) return false;
  if ((uint64_t)off + n > it->second.size()) return false;
  memcpy(buf, it->second.data() + off, n); return true;
}
static rmp_arenas ar;
static uint8_t *f_alloc(uint32_t bytes) { return rmp_arenas_alloc(&ar, bytes); }
static void f_reset() { rmp_arenas_reset(&ar); }
static void f_sync(void *, uint32_t) {}
static void f_log(const char *m) { printf("  log: %s\n", m); }
static const rmp_platform pf = {f_size_of, f_read_at, f_alloc, f_reset, f_sync, f_log, nullptr};

int main(int argc, char **argv) {
  // ---- the bump arenas ----
  static uint8_t in_mem[32768], ps_mem[65536];
  ar = rmp_arenas{{{in_mem, 32768, 0}, {nullptr, 0, 0}}};
  CHECK(rmp_arenas_alloc(&ar, 32768) == in_mem && !rmp_arenas_alloc(&ar, 1));    // no PSRAM: full is full
  CHECK(!rmp_arenas_alloc_in(&ar, RMP_PLACE_PSRAM, 8) && !rmp_arenas_alloc_in(&ar, 7, 8));
  rmp_arenas_reset(&ar);
  CHECK(ar.a[0].used == 0 && rmp_arenas_alloc(&ar, 3) == in_mem && rmp_arenas_alloc(&ar, 8) == in_mem + 8);   // 8-aligned
  CHECK(rmp_arenas_where(&ar, in_mem + 5) == RMP_PLACE_INTERNAL && rmp_arenas_where(&ar, ps_mem) == RMP_PLACE_NONE);
  CHECK(rmp_arenas_where(&ar, in_mem + 32768) == RMP_PLACE_NONE);
  ar.a[1] = rmp_bump{ps_mem, 65536, 0};
  rmp_arenas_reset(&ar);
  CHECK(rmp_arenas_alloc(&ar, 20000) == in_mem);                 // internal first
  CHECK(rmp_arenas_alloc(&ar, 20000) == ps_mem);                 // overflow: PSRAM
  CHECK(rmp_arenas_alloc(&ar, 12000) == in_mem + 20000);         // a smaller one still fits internally
  CHECK(rmp_arenas_alloc(&ar, 40000) == ps_mem + 20000);
  CHECK(!rmp_arenas_alloc(&ar, 8000));                           // both full: NO_ROOM
  CHECK(rmp_arenas_where(&ar, ps_mem + 100) == RMP_PLACE_PSRAM);
  CHECK(rmp_arenas_alloc_in(&ar, RMP_PLACE_PSRAM, 5536) == ps_mem + 60000 && !rmp_arenas_alloc_in(&ar, RMP_PLACE_PSRAM, 1));
  rmp_arenas_reset(&ar);
  CHECK(ar.a[0].used == 0 && ar.a[1].used == 0 && rmp_arenas_alloc(&ar, 40000) == ps_mem);   // too big for internal: PSRAM
  CHECK(!strcmp(rmp_place_name(RMP_PLACE_INTERNAL), "internal") && !strcmp(rmp_place_name(RMP_PLACE_PSRAM), "psram"));
  CHECK(!rmp_bump_alloc(&ar.a[0], 0xFFFFFFF0u));

  // ---- MPU windows: the DTCM arena's (64 KB windows, 8 KB subregions), as rmp_arena.cpp had them ----
  CHECK(rmp_mpu_srd(0x20000000u, 8192, 0x20000000u, 0x20008000u) == 0xF0u);   // first 4 subregions on
  CHECK(rmp_mpu_srd(0x20010000u, 8192, 0x20000000u, 0x20008000u) == 0xFFu);   // second window unused
  CHECK(rmp_mpu_srd(0x20000000u, 8192, 0x2000C000u, 0x20014000u) == 0x3Fu);   // straddling: 6, 7 here...
  CHECK(rmp_mpu_srd(0x20010000u, 8192, 0x2000C000u, 0x20014000u) == 0xFCu);   // ...0, 1 in the next
  // the PSRAM arena: R = 1 MB, sub 128 KB, R-aligned windows; every sub-aligned start covered exactly
  for (uint32_t a = 0x70000000u; a < 0x70200000u; a += 0x20000u) {
    const uint32_t r = 1u << 20, sub = r / 8, e = a + r, w0 = a & ~(r - 1), w1 = w0 + r;
    uint32_t s0 = rmp_mpu_srd(w0, sub, a, e), s1 = rmp_mpu_srd(w1, sub, a, e), on = 0;
    for (int k = 0; k < 8; ++k) on += !(s0 >> k & 1) + !(s1 >> k & 1);
    CHECK(on == 8);                                              // exactly R bytes enabled
    CHECK(!(s0 & 1u << ((a - w0) / sub)));                       // the first byte's subregion on
  }

  // ---- limits: internal unchanged, PSRAM scaled + the cold allowance ----
  CHECK(rmp_limit_budget(32000, RMP_PLACE_INTERNAL) == 32000 && rmp_limit_hard(32000, RMP_PLACE_INTERNAL) == 32000);
  CHECK(rmp_limit_hard(32000, RMP_PLACE_NONE) == 32000);
  CHECK(rmp_limit_budget(32000, RMP_PLACE_PSRAM) == 32000 * RMP_PSRAM_LIMIT_X);
  CHECK(rmp_limit_hard(32000, RMP_PLACE_PSRAM) == 32000 * RMP_PSRAM_LIMIT_X + RMP_PSRAM_COLD_CYCLES);
  CHECK(rmp_limit_hard(0, RMP_PLACE_PSRAM) == 0);                // 0 = no limit stays no limit
  CHECK(rmp_limit_hard(0x7FFFFFF0u, RMP_PLACE_PSRAM) == 0x7FFFFFFFu);

  // ---- rmp_slots_load over two arenas ----
  auto pkg = read_file(argc > 1 ? argv[1] : "../../../../plugins/sine_test/sine_test.rmp");
  rmp_info info; CHECK(rmp_verify_header(pkg.data(), (uint32_t)pkg.size(), &info) == RMP_OK);
  const uint32_t need = info.image_size + info.bss_size;
  const char *ids[5] = {"rytm.sine_test", "rytm.a", "rytm.b", "rytm.c", "rytm.d"};
  for (int k = 0; k < 5; ++k) { auto c = pkg; set_id(c, ids[k]); store[ids[k]] = c; }
  rmp_slot_map m; memset(&m, 0, sizeof m);
  for (int k = 0; k < 5; ++k) { strcpy(m.slot[k].id, ids[k]); m.slot[k].version = info.version; }
  static uint8_t in2[65536], ps2[65536];
  const uint32_t step = (need + 7u) & ~7u;
  // internal room for 2, PSRAM for 2: U1, U2 internal, U3, U4 PSRAM, U5 NO_ROOM
  rmp_loaded out[RMP_SLOTS];
  ar = rmp_arenas{{{in2, 2 * step, 0}, {ps2, 2 * step, 0}}};
  rmp_slots_load(&pf, &m, out);
  for (int s = 0; s < 4; ++s) CHECK(out[s].state == RMP_SLOT_READY);
  CHECK(rmp_arenas_where(&ar, out[0].plugin) == RMP_PLACE_INTERNAL && rmp_arenas_where(&ar, out[1].plugin) == RMP_PLACE_INTERNAL);
  CHECK(rmp_arenas_where(&ar, out[2].plugin) == RMP_PLACE_PSRAM && rmp_arenas_where(&ar, out[3].plugin) == RMP_PLACE_PSRAM);
  CHECK(out[4].state == RMP_SLOT_NO_ROOM && out[4].need == need);
  // the same map without a PSRAM arena: exactly the single arena's states (2 READY, 3 NO_ROOM, same need)
  rmp_loaded one[RMP_SLOTS];
  ar = rmp_arenas{{{in2, 2 * step, 0}, {nullptr, 0, 0}}};
  rmp_slots_load(&pf, &m, one);
  CHECK(one[0].state == RMP_SLOT_READY && one[1].state == RMP_SLOT_READY);
  for (int s = 2; s < 5; ++s) CHECK(one[s].state == RMP_SLOT_NO_ROOM && one[s].need == need);
  CHECK(ar.a[0].used == 2 * step - (step - need) && ar.a[1].used == 0);
  // a full reload resets both arenas: the same placement again, no growth
  ar = rmp_arenas{{{in2, 2 * step, 0}, {ps2, 2 * step, 0}}};
  rmp_slots_load(&pf, &m, out);
  uint32_t u0 = ar.a[0].used, u1 = ar.a[1].used;
  rmp_slots_load(&pf, &m, out);
  CHECK(ar.a[0].used == u0 && ar.a[1].used == u1);
  // incremental: U5 reassigned while U1..U4 play - NO_ROOM (both full), the caller's fallback is a full reload
  rmp_slots_load_one(&pf, &m, 4, out);
  CHECK(out[4].state == RMP_SLOT_NO_ROOM);
  // a bigger PSRAM arena: the incremental load lands in PSRAM, the others keep their images
  ar.a[1].size = 3 * step;
  const rmp_plugin *keep = out[0].plugin;
  rmp_slots_load_one(&pf, &m, 4, out);
  CHECK(out[4].state == RMP_SLOT_READY && rmp_arenas_where(&ar, out[4].plugin) == RMP_PLACE_PSRAM && out[0].plugin == keep);
  // a package too big for the internal arena alone goes to PSRAM even when the internal one is empty
  ar = rmp_arenas{{{in2, need - 8, 0}, {ps2, 65536, 0}}};
  rmp_slot_map m1; memset(&m1, 0, sizeof m1); m1.slot[0] = m.slot[0];
  rmp_slots_load(&pf, &m1, out);
  CHECK(out[0].state == RMP_SLOT_READY && rmp_arenas_where(&ar, out[0].plugin) == RMP_PLACE_PSRAM && ar.a[0].used == 0);
  printf("ok\n");
  return 0;
}
