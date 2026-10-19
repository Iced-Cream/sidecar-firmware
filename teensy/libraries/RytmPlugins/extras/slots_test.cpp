// extras/slots_test.cpp - slot maps and loading decisions on the Mac.
// build: c++ -std=c++17 -O1 -Wall -DNDEBUG -I../src slots_test.cpp ../src/rmp_slots.cpp ../src/rmp_loader.cpp -o /tmp/slots_test && /tmp/slots_test
// (run from extras/; argv[1] overrides the path of sine_test.rmp)
//
// rmp_slots_load never executes plugin code (init() is the caller's, through the guard): it only
// reads the entry table's magic and api, which are data, so it runs on the Mac.
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "rmp_slots.h"
#include "rmp_api.h"

static std::vector<uint8_t> read_file(const char *p) {
  FILE *f = fopen(p, "rb"); CHECK(f);
  std::vector<uint8_t> v; uint8_t b[4096]; size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
  fclose(f); return v;
}

static void refresh_crc(std::vector<uint8_t> &p) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = RMP_HEADER_BYTES; i < p.size(); ++i) { c ^= p[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
  rmp_header h; memcpy(&h, p.data(), sizeof h); h.crc32 = ~c; memcpy(p.data(), &h, sizeof h);
}

// ---- the fake platform: an in-memory store and a malloc'd arena ----
static std::map<std::string, std::vector<uint8_t>> store;
static int reads, alloc_calls;
static bool f_size_of(const char *id, uint32_t *len) {
  auto it = store.find(id); if (it == store.end()) return false;
  *len = (uint32_t)it->second.size(); return true;
}
static bool f_read_at(const char *id, uint32_t off, uint8_t *buf, uint32_t n) {
  auto it = store.find(id); if (it == store.end()) return false;
  if ((uint64_t)off + n > it->second.size()) return false;
  ++reads; memcpy(buf, it->second.data() + off, n); return true;
}
// The arena is a malloc'd 32 KB block. rmp_slots_load places the image for base
// (uint32_t)(uintptr_t)img, truncated on a 64-bit Mac: that only changes the relocated
// pointers, never the magic/api words these tests read, so it doesn't matter here.
static uint8_t *arena; static uint32_t arena_cap = 32768, arena_used, allocs;
static uint8_t *f_alloc(uint32_t bytes) {
  ++alloc_calls;
  uint32_t at = (arena_used + 7u) & ~7u;
  if (bytes > arena_cap || at > arena_cap - bytes) return nullptr;
  arena_used = at + bytes; ++allocs; return arena + at;
}
static void f_reset() { arena_used = 0; allocs = 0; alloc_calls = 0; reads = 0; memset(arena, 0xEE, 32768); }
static void f_sync(void *, uint32_t) {}
static void f_log(const char *m) { printf("  log: %s\n", m); }
static const rmp_platform pf = {f_size_of, f_read_at, f_alloc, f_reset, f_sync, f_log, nullptr};

static bool parse(const char *t, rmp_slot_map *m) { return rmp_slots_parse(t, (uint32_t)strlen(t), m); }
static bool map_zero(const rmp_slot_map &m) {
  static const rmp_slot_map z = {};
  return !memcmp(&m, &z, sizeof m);
}

int main(int argc, char **argv) {
  rmp_slot_map m;
  // ---- the slot map file: one line per slot "U<n> <id> <a.b.c>" ----
  const char *txt = "U1 rytm.sine_test 1.0.0\nU3 befaco.np.bank1 1.2.0\n";
  CHECK(parse(txt, &m));
  CHECK(!strcmp(m.slot[0].id, "rytm.sine_test") && m.slot[0].version == 0x010000);
  CHECK(m.slot[1].id[0] == 0 && m.slot[3].id[0] == 0 && m.slot[4].id[0] == 0);
  CHECK(!strcmp(m.slot[2].id, "befaco.np.bank1") && m.slot[2].version == 0x010200);
  char out[256]; uint32_t n = rmp_slots_format(&m, out, sizeof out);
  CHECK(n == strlen(txt) && !strcmp(out, txt));
  CHECK(rmp_slots_format(&m, out, 10) == 0);              // does not fit
  // accepted: empty file, blank/whitespace lines, CRLF, no final newline, U5, 255
  CHECK(parse("", &m) && map_zero(m));
  CHECK(parse("\n  \n\t\r\n", &m) && map_zero(m));
  CHECK(parse("U1 a.b 1.2.3\r\nU5 c 255.0.255", &m));
  CHECK(!strcmp(m.slot[0].id, "a.b") && m.slot[0].version == 0x010203);
  CHECK(!strcmp(m.slot[4].id, "c") && m.slot[4].version == 0xFF00FF);
  CHECK(parse("U2  x\t 0.0.1  \r", &m) && !strcmp(m.slot[1].id, "x") && m.slot[1].version == 1);
  CHECK(parse("U4 0123456789012345678901234567890 1.0.0\n", &m));   // 31-char id
  CHECK(parse("U1 a 0.0.0\nU2 b 10.200.0", &m) && m.slot[0].version == 0 && m.slot[1].version == 0x0AC800);
  { static const char nul[] = "U1 a\0b 1.0.0\n";                    // a NUL inside the id token
    memset(&m, 0x5A, sizeof m); CHECK(!rmp_slots_parse(nul, sizeof nul - 1, &m) && map_zero(m)); }
  // refused (and m left all zero): every one of these
  const char *bad[] = {
    "garbage\n", "U1 a 1.0.0\nhello\n", "u1 a 1.0.0", "U a 1.0.0", "U0 a 1.0.0", "U6 a 1.0.0", "U12 a 1.0.0",
    "U1 a 1.0.0\nU1 b 1.0.0\n",                                  // duplicate slot
    "U1 a/b 1.0.0", "U1 .a 1.0.0", "U1 a..b 1.0.0", "U1 a\\b 1.0.0",
    "U1 01234567890123456789012345678901 1.0.0",                 // 32-char id
    "U1 a 1.0", "U1 a 1.0.", "U1 a .0.0", "U1 a 1..0", "U1 a", "U1 a ", "U1",
    "U1 a 256.0.0", "U1 a 1.0.256", "U1 a 1.0.0.0", "U1 a 1.0.0 extra", "U1 a 1.0.0x", "U1 a 1.0.-1",
    "U1 a 0001.0.0", "U1 a 01.0.0", "U1 a 1.00.0", "U1 a 1.0.07",   // leading zeros: the round trip must be exact
    "U1 a\x01b 1.0.0", "U1 a\x7f 1.0.0", "U1 caf\xc3\xa9 1.0.0",       // control, DEL, non-ASCII in the id
  };
  for (const char *b : bad) {
    memset(&m, 0x5A, sizeof m);
    bool ok = parse(b, &m);
    if (ok || !map_zero(m)) fprintf(stderr, "accepted or not zeroed: \"%s\"\n", b);
    CHECK(!ok && map_zero(m));
  }
  // flags: U1..U5 -> 0x50, 0x58, 0x60, 0x68, 0x70
  CHECK(rmp_slot_of_flag(0x50) == 0 && rmp_slot_of_flag(0x58) == 1 && rmp_slot_of_flag(0x70) == 4);
  CHECK(rmp_slot_of_flag(0x48) == -1 && rmp_slot_of_flag(0x78) == -1 && rmp_slot_of_flag(0x51) == -1);

  // ---- loading ----
  auto pkg = read_file(argc > 1 ? argv[1] : "../../../../plugins/sine_test/sine_test.rmp");
  arena = (uint8_t *)malloc(32768); CHECK(arena);
  rmp_loaded ld[RMP_SLOTS];
  auto load = [&](const char *map_txt) {
    rmp_slot_map mm; CHECK(parse(map_txt, &mm)); rmp_slots_load(&pf, &mm, ld);
  };
  store.clear(); store["rytm.sine_test"] = pkg;
  // empty map: nothing loaded, the arena is reset
  arena_used = 123; load("");
  for (int s = 0; s < RMP_SLOTS; ++s) CHECK(ld[s].state == RMP_SLOT_EMPTY && ld[s].plugin == nullptr);
  CHECK(arena_used == 0 && allocs == 0);
  // READY: the entry table is in the arena, magic and api right
  load("U2 rytm.sine_test 1.0.0\n");
  CHECK(ld[1].state == RMP_SLOT_READY && ld[1].version == 0x010000 && ld[1].plugin);
  CHECK(ld[1].plugin->magic == RMP_PLUGIN_MAGIC && ld[1].plugin->api == 1);   // sine_test is an API-1 plugin
  CHECK((const uint8_t *)ld[1].plugin >= arena && (const uint8_t *)ld[1].plugin < arena + arena_used);
  CHECK(ld[0].state == RMP_SLOT_EMPTY && ld[2].state == RMP_SLOT_EMPTY);
  uint32_t one = arena_used; CHECK(allocs == 1 && one >= 4080);   // sine_test: image 4080 + bss 8
  // two slots, same id: one image, shared
  load("U1 rytm.sine_test 1.0.0\nU4 rytm.sine_test 1.0.0\n");
  CHECK(ld[0].state == RMP_SLOT_READY && ld[3].state == RMP_SLOT_READY && ld[0].plugin == ld[3].plugin);
  CHECK(allocs == 1 && arena_used == one);
  // same id, one slot's version out of range: that slot alone is MISSING, still one image
  load("U1 rytm.sine_test 2.0.0\nU2 rytm.sine_test 1.0.0\nU3 rytm.sine_test 0.9.0\n");
  CHECK(ld[0].state == RMP_SLOT_MISSING && ld[1].state == RMP_SLOT_READY && ld[2].state == RMP_SLOT_MISSING);
  CHECK(allocs == 1 && ld[0].plugin == nullptr);
  // MISSING: not installed
  load("U1 rytm.nothing 1.0.0\nU2 rytm.sine_test 1.0.0\n");
  CHECK(ld[0].state == RMP_SLOT_MISSING && ld[1].state == RMP_SLOT_READY);
  // MISSING: the saved version is newer than the installed one, or older than compatible_from
  load("U1 rytm.sine_test 2.0.0\n"); CHECK(ld[0].state == RMP_SLOT_MISSING && allocs == 0);
  load("U1 rytm.sine_test 0.9.9\n"); CHECK(ld[0].state == RMP_SLOT_MISSING && allocs == 0);
  // BAD: corrupt body (CRC), checked before anything is allocated
  { auto c = pkg; c[200] ^= 1; store["rytm.sine_test"] = c; }
  load("U1 rytm.sine_test 1.0.0\n"); CHECK(ld[0].state == RMP_SLOT_BAD && allocs == 0);
  // a per-id failure is remembered: a CRC-valid package whose entry magic is broken
  // (fails AFTER allocation) in all five slots -> all BAD, one allocation, one load
  { auto c = pkg; rmp_header h; memcpy(&h, c.data(), sizeof h);
    c[RMP_HEADER_BYTES + h.entry_offset] ^= 0xFF; refresh_crc(c); store["rytm.sine_test"] = c; }
  load("U1 rytm.sine_test 1.0.0\nU2 rytm.sine_test 1.0.0\nU3 rytm.sine_test 1.0.0\nU4 rytm.sine_test 1.0.0\nU5 rytm.sine_test 1.0.0\n");
  for (int s = 0; s < RMP_SLOTS; ++s) CHECK(ld[s].state == RMP_SLOT_BAD && ld[s].plugin == nullptr);
  CHECK(allocs == 1 && alloc_calls == 1);
  // a bad-CRC package in two slots is read and CRC'd once
  { auto c = pkg; c[200] ^= 1; store["rytm.sine_test"] = c; }
  load("U1 rytm.sine_test 1.0.0\n"); int one_reads = reads; CHECK(ld[0].state == RMP_SLOT_BAD && one_reads > 1);
  load("U1 rytm.sine_test 1.0.0\nU3 rytm.sine_test 1.0.0\n");
  CHECK(ld[0].state == RMP_SLOT_BAD && ld[2].state == RMP_SLOT_BAD && reads == one_reads && alloc_calls == 0);
  store["rytm.sine_test"] = pkg;
  // BAD: a package stored under another id than its header's; a truncated package
  store["rytm.sine_test"] = pkg; store["rytm.other"] = pkg;
  { auto c = pkg; c.resize(c.size() - 4); store["rytm.short"] = c; }
  load("U1 rytm.other 1.0.0\nU2 rytm.short 1.0.0\nU3 rytm.sine_test 1.0.0\n");
  CHECK(ld[0].state == RMP_SLOT_BAD && ld[1].state == RMP_SLOT_BAD && ld[2].state == RMP_SLOT_READY);
  // NO_ROOM: a tiny arena
  arena_cap = 1024;
  load("U1 rytm.sine_test 1.0.0\n"); CHECK(ld[0].state == RMP_SLOT_NO_ROOM && ld[0].plugin == nullptr);
  load("U1 rytm.sine_test 1.0.0\nU2 rytm.sine_test 1.0.0\n");   // NO_ROOM remembered too: one attempt
  CHECK(ld[0].state == RMP_SLOT_NO_ROOM && ld[1].state == RMP_SLOT_NO_ROOM && alloc_calls == 1);
  CHECK(ld[0].need == 4088 && ld[1].need == 4088);   // what it asked for (image 4080 + bss 8): the web page's "needs N B"
  // ... but a version-range MISSING is per slot: a later slot with a good version still loads
  arena_cap = 32768;
  load("U1 rytm.sine_test 9.0.0\nU2 rytm.sine_test 1.0.0\n");
  CHECK(ld[0].state == RMP_SLOT_MISSING && ld[1].state == RMP_SLOT_READY && allocs == 1);
  // an unsafe id from a map built in code (not parsed) never reaches the platform
  { rmp_slot_map mm = {}; strcpy(mm.slot[0].id, "../x"); mm.slot[0].version = 0x010000;
    reads = 0; rmp_slots_load(&pf, &mm, ld); CHECK(ld[0].state == RMP_SLOT_MISSING && reads == 0); }
  // ---- incremental: one slot appended, the others untouched ----
  arena_cap = 32768;
  { store["rytm.sine_test"] = pkg; store["rytm.other"] = pkg;
    { auto c = pkg; rmp_header h; memcpy(&h, c.data(), sizeof h); strcpy(h.id, "rytm.other"); memcpy(c.data(), &h, sizeof h);
      refresh_crc(c); store["rytm.other"] = c; }
    rmp_slot_map mm; CHECK(parse("U1 rytm.sine_test 1.0.0\n", &mm)); rmp_loaded L[RMP_SLOTS];
    rmp_slots_load(&pf, &mm, L); CHECK(L[0].state == RMP_SLOT_READY);
    const rmp_plugin *p0 = L[0].plugin; uint32_t used0 = arena_used;
    // a second slot with a different id: appended, slot 0 untouched
    CHECK(parse("U1 rytm.sine_test 1.0.0\nU2 rytm.other 1.0.0\n", &mm));
    allocs = 0; rmp_slots_load_one(&pf, &mm, 1, L);
    CHECK(L[1].state == RMP_SLOT_READY && L[1].plugin != p0 && L[0].plugin == p0 && L[0].state == RMP_SLOT_READY);
    CHECK(allocs == 1 && arena_used > used0);
    // the same id as a READY slot: shared, no read, no arena
    CHECK(parse("U1 rytm.sine_test 1.0.0\nU2 rytm.other 1.0.0\nU3 rytm.sine_test 1.0.0\n", &mm));
    reads = 0; allocs = 0; uint32_t used1 = arena_used; rmp_slots_load_one(&pf, &mm, 2, L);
    CHECK(L[2].state == RMP_SLOT_READY && L[2].plugin == p0 && reads == 0 && allocs == 0 && arena_used == used1);
    // ... with its own version check (outside the package's range: MISSING, nothing else changes)
    CHECK(parse("U1 rytm.sine_test 1.0.0\nU2 rytm.other 1.0.0\nU3 rytm.sine_test 9.0.0\n", &mm));
    rmp_slots_load_one(&pf, &mm, 2, L); CHECK(L[2].state == RMP_SLOT_MISSING && L[0].state == RMP_SLOT_READY);
    // replace slot 1 by the sine: shared with slot 0; the old image stays allocated
    CHECK(parse("U1 rytm.sine_test 1.0.0\nU2 rytm.sine_test 1.0.0\n", &mm));
    rmp_slots_load_one(&pf, &mm, 1, L); CHECK(L[1].plugin == p0 && arena_used == used1);
    // empty / not installed / bad slots
    CHECK(parse("U1 rytm.sine_test 1.0.0\nU2 rytm.none 1.0.0\n", &mm));
    rmp_slots_load_one(&pf, &mm, 1, L); CHECK(L[1].state == RMP_SLOT_MISSING && L[0].plugin == p0);
    CHECK(parse("U1 rytm.sine_test 1.0.0\n", &mm));
    rmp_slots_load_one(&pf, &mm, 1, L); CHECK(L[1].state == RMP_SLOT_EMPTY);
    // stepped: LOADING until done; several passes with a small CRC budget
    CHECK(parse("U1 rytm.sine_test 1.0.0\nU2 rytm.other 1.0.0\n", &mm));
    { rmp_load_job j; int passes = 1;
      CHECK(!rmp_slots_one_begin(&j, &pf, &mm, 1, L) && L[1].state == RMP_SLOT_LOADING);
      while (!rmp_slots_one_step(&j, L, 1024)) { ++passes; CHECK(L[1].state == RMP_SLOT_LOADING && L[0].state == RMP_SLOT_READY); }
      CHECK(L[1].state == RMP_SLOT_READY && passes >= 5); }
    { rmp_load_job j; CHECK(rmp_slots_one_begin(&j, &pf, &mm, 4, L) && rmp_slots_one_step(&j, L, 1024)); }   // a step after a true begin is a no-op
    CHECK(L[0].state == RMP_SLOT_READY);
    // no room: NO_ROOM for the slot (the caller then reloads fully), the others stay
    arena_cap = arena_used + 8;
    { auto c = pkg; rmp_header h; memcpy(&h, c.data(), sizeof h); strcpy(h.id, "rytm.third"); memcpy(c.data(), &h, sizeof h);
      refresh_crc(c); store["rytm.third"] = c; }
    CHECK(parse("U1 rytm.sine_test 1.0.0\nU2 rytm.other 1.0.0\nU4 rytm.third 1.0.0\n", &mm));
    rmp_slots_load_one(&pf, &mm, 3, L); CHECK(L[3].state == RMP_SLOT_NO_ROOM && L[0].state == RMP_SLOT_READY && L[1].state == RMP_SLOT_READY);
    arena_cap = 32768; }
  // the entry table's api must be in range AND equal the header's (the header's is range-checked by
  // rmp_verify_header): header 1 over entry 3 (out of range) and header 1 over entry 2 (in range,
  // but not the header's) are both BAD "bad entry"
  for (uint32_t entry_api : {3u, 2u}) {
    auto t = pkg;
    rmp_header h; memcpy(&h, t.data(), sizeof h); CHECK(h.api == 1);
    memcpy(&t[RMP_HEADER_BYTES + h.entry_offset + 4], &entry_api, 4);
    refresh_crc(t);
    store.clear(); store["rytm.sine_test"] = t;
    load("U1 rytm.sine_test 1.0.0\n");
    CHECK(ld[0].state == RMP_SLOT_BAD && ld[0].plugin == nullptr);
    store.clear(); store["rytm.sine_test"] = pkg;
  }
  load("U1 rytm.sine_test 1.0.0\n"); CHECK(ld[0].state == RMP_SLOT_READY);   // the untouched package still loads
  free(arena);
  printf("ok\n");
  return 0;
}
