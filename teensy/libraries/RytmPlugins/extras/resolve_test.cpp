// extras/resolve_test.cpp - which machine a user slot plays when a project loads (rmp_resolve), on the Mac.
// build: c++ -std=c++17 -O1 -Wall -I../src resolve_test.cpp ../src/rmp_resolve.cpp ../src/rmp_slots.cpp ../src/rmp_proto.cpp ../src/rmp_manifest.cpp ../src/rmp_loader.cpp -o /tmp/resolve_test && /tmp/resolve_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "rmp_proto.h"
#include "rmp_resolve.h"

static rmp_pkg_ref pk[4];
static int npk;
static void installed(const char *ids) {   // space separated, version 1.2.0 each, name = id upper-cased tail
  static char buf[256], names[4][11];
  strncpy(buf, ids, sizeof buf - 1);
  npk = 0;
  for (char *t = strtok(buf, " "); t && npk < 4; t = strtok(nullptr, " ")) {
    const char *d = strchr(t, '.');
    snprintf(names[npk], 11, "%s", d ? d + 1 : t);
    for (char *c = names[npk]; *c; ++c) *c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    pk[npk] = rmp_pkg_ref{t, 0x010200, rmp_plugin_fp(t), names[npk]};
    ++npk;
  }
}
static void put(rmp_slot_map *m, int s, const char *id) {
  memset(&m->slot[s], 0, sizeof m->slot[s]);
  if (id) { strncpy(m->slot[s].id, id, 31); m->slot[s].version = 0x010000; }
}
static rmp_prints prints(std::initializer_list<rmp_print> e) {
  rmp_prints p;
  memset(&p, 0, sizeof p);
  for (const rmp_print &x : e) p.e[p.n++] = x;
  return p;
}
static uint16_t fp(const char *id) { return rmp_plugin_fp(id); }

int main() {
  rmp_slot_map m, prev;
  rmp_slot_notes n;

  // ---- the message: built as sim/protocol.py slot_prints([(1, fp(sc.acid), 12), (3, fp(sc.grain), 2)], seq=5)
  {
    const uint8_t msg[] = {0xF0, 0x7D, 0x52, 0x54, 0x25, 0x01, 0x05, 0x00, 0x02, 0x01, 0x11, 0x40, 0x0C,
                           0x03, 0x0E, 0x25, 0x02, 0xF7};
    rmp_prints p;
    CHECK(rmp_parse_prints(msg, sizeof msg, &p));
    CHECK(p.seq == 5 && p.n == 2 && p.e[0].slot == 0 && p.e[0].fp == fp("sc.acid") && p.e[0].count == 12);
    CHECK(p.e[1].slot == 2 && p.e[1].fp == fp("sc.grain") && p.e[1].count == 2);
    CHECK(fp("sc.acid") == 2240);   // the shared vector (sim/protocol.py plugin_fp)
    uint8_t bad[sizeof msg];
    memcpy(bad, msg, sizeof msg); bad[5] = 2; CHECK(!rmp_parse_prints(bad, sizeof bad, &p));       // another version
    memcpy(bad, msg, sizeof msg); bad[8] = 3; CHECK(!rmp_parse_prints(bad, sizeof bad, &p));       // wrong length
    memcpy(bad, msg, sizeof msg); bad[9] = 6; CHECK(!rmp_parse_prints(bad, sizeof bad, &p));       // slot 6
    memcpy(bad, msg, sizeof msg); bad[12] = 0; CHECK(!rmp_parse_prints(bad, sizeof bad, &p));      // count 0
    const uint8_t none[] = {0xF0, 0x7D, 0x52, 0x54, 0x25, 0x01, 0x00, 0x00, 0x00, 0xF7};
    CHECK(rmp_parse_prints(none, sizeof none, &p) && p.n == 0);
    rmp_prints q = p; q.seq = 9; CHECK(rmp_prints_hash(&p) == rmp_prints_hash(&q));   // seq is not content
  }

  // ---- the note file round trip, an older map without notes
  {
    memset(&n, 0, sizeof n);
    n.slot[0] = rmp_slot_note{RMP_SRC_SOUNDS, RMP_RF_MIXED, 2240, 3, "sc.acid", 0x010200, "ACID"};
    n.slot[2] = rmp_slot_note{RMP_SRC_EMPTY, 0, 0, 0, "", 0, ""};
    n.slot[4] = rmp_slot_note{RMP_SRC_NONE, RMP_RF_UNKNOWN, 77, 1, "x.pad", 0x000900, "MY PAD"};
    char txt[400];
    uint32_t len = rmp_notes_format(&n, txt, sizeof txt);
    CHECK(len > 0 && strstr(txt, "U1 sounds 8 2240 sc.acid 1.2.0 ACID\n") && strstr(txt, "U3 empty 0 0 - -\n") &&
          strstr(txt, "U5 none 2 77 x.pad 0.9.0 MY PAD\n"));
    rmp_slot_notes r;
    CHECK(rmp_notes_parse(txt, len, &r));
    CHECK(r.slot[0].src == RMP_SRC_SOUNDS && r.slot[0].fp == 2240 && !strcmp(r.slot[0].id, "sc.acid") && !strcmp(r.slot[0].name, "ACID"));
    CHECK(r.slot[2].src == RMP_SRC_EMPTY && r.slot[4].version == 0x000900 && !strcmp(r.slot[4].name, "MY PAD"));
    CHECK(r.slot[1].src == RMP_SRC_NONE && !r.slot[1].id[0]);
    const char junk[] = "U9 saved 0 0 - -\nU2 bogus 0 0 - -\nhello\nU4 saved 0 0 a.b 1.0.0\n";
    CHECK(rmp_notes_parse(junk, sizeof junk - 1, &r) && r.slot[3].src == RMP_SRC_SAVED && r.slot[1].src == RMP_SRC_NONE);
    memset(&m, 0, sizeof m);
    put(&m, 1, "sc.acid");
    CHECK(!rmp_resolve_open(&m, &n, true, false));   // a map from an older build: its slots saved, nothing fresh
    CHECK(n.slot[1].src == RMP_SRC_SAVED && n.slot[0].src == RMP_SRC_NONE);
  }

  installed("sc.acid sc.grain sc.metal");

  // ---- own map: slots with no fingerprint information keep it; a fingerprint report never touches them
  {
    memset(&m, 0, sizeof m);
    put(&m, 0, "sc.metal");
    CHECK(!rmp_resolve_open(&m, &n, true, false));
    rmp_prints p = prints({{1, fp("sc.grain"), 4}});
    CHECK(rmp_resolve_prints(&m, &n, &p, pk, npk) == 2u);   // U2 from the sounds, U1 untouched
    CHECK(!strcmp(m.slot[0].id, "sc.metal") && n.slot[0].src == RMP_SRC_SAVED && !(n.slot[0].flags & RMP_RF_UPDATED));
    CHECK(!strcmp(m.slot[1].id, "sc.grain") && m.slot[1].version == 0x010200 && n.slot[1].src == RMP_SRC_SOUNDS);
    CHECK(n.slot[1].fp == fp("sc.grain") && n.slot[1].nfp == 4 && !strcmp(n.slot[1].name, "GRAIN"));
  }

  // ---- fingerprint match on a shared project (no map, no notes): fresh; prints fill, the rest inherits
  {
    memset(&prev, 0, sizeof prev);
    put(&prev, 0, "sc.metal"); put(&prev, 3, "sc.acid");
    memset(&m, 0, sizeof m);
    CHECK(rmp_resolve_open(&m, &n, false, false));
    rmp_prints p = prints({{0, fp("sc.acid"), 7}, {2, fp("sc.grain"), 1}});
    CHECK(rmp_resolve_prints(&m, &n, &p, pk, npk) == 5u);
    CHECK(rmp_resolve_inherit(&m, &n, &prev) == 8u);   // U4 inherits; U1 has fingerprints: not inherited
    CHECK(!strcmp(m.slot[0].id, "sc.acid") && !strcmp(m.slot[2].id, "sc.grain") && !strcmp(m.slot[3].id, "sc.acid"));
    CHECK(n.slot[3].src == RMP_SRC_INHERITED && n.slot[0].src == RMP_SRC_SOUNDS && !m.slot[1].id[0]);
  }

  // ---- inherit without any report (an older Rytm): every used slot of the previous project
  {
    memset(&m, 0, sizeof m);
    CHECK(rmp_resolve_open(&m, &n, false, false));
    CHECK(rmp_resolve_inherit(&m, &n, &prev) == 9u && !strcmp(m.slot[0].id, "sc.metal"));
    // the next load of this project: its own (inherited, saved) map - saved now, not fresh, not replaced lazily
    {
      char t2[200];
      rmp_slot_notes r2;
      rmp_slot_map m2 = m;
      CHECK(rmp_notes_parse(t2, rmp_notes_format(&n, t2, sizeof t2), &r2));
      CHECK(!rmp_resolve_open(&m2, &r2, true, true) && r2.slot[0].src == RMP_SRC_SAVED);
      CHECK(!rmp_resolve_tag(&m2, &r2, 0, fp("sc.grain"), pk, npk) && !strcmp(m2.slot[0].id, "sc.metal"));
      // a stale "empty" note beside a used map (an older build edited the map): the map wins
      r2.slot[1].src = RMP_SRC_EMPTY;
      put(&m2, 1, "sc.acid");
      CHECK(!rmp_resolve_open(&m2, &r2, true, true) && !strcmp(m2.slot[1].id, "sc.acid") && r2.slot[1].src == RMP_SRC_SAVED);
    }
    rmp_slot_notes &r = n;
    // within the load: a trig tagged for another machine replaces an inherited slot, never a saved one
    CHECK(rmp_resolve_tag(&m, &r, 0, fp("sc.grain"), pk, npk) && !strcmp(m.slot[0].id, "sc.grain") &&
          r.slot[0].src == RMP_SRC_SOUNDS && (r.slot[0].flags & RMP_RF_UPDATED));
    r.slot[3].src = RMP_SRC_SAVED;
    CHECK(!rmp_resolve_tag(&m, &r, 3, fp("sc.metal"), pk, npk) && !strcmp(m.slot[3].id, "sc.acid"));
    CHECK(!rmp_resolve_tag(&m, &r, 0, fp("sc.grain"), pk, npk));   // matches now: nothing
    // an empty slot fills lazily; a tag no package has is noted
    CHECK(rmp_resolve_tag(&m, &r, 1, fp("sc.metal"), pk, npk) && !strcmp(m.slot[1].id, "sc.metal"));
    CHECK(!rmp_resolve_tag(&m, &r, 2, 1234, pk, npk) && (r.slot[2].flags & RMP_RF_UNKNOWN) && r.slot[2].fp == 1234);
  }

  // ---- collision: two installed packages share a fingerprint
  {
    // find another id with sc.acid's fingerprint (FNV-1a mod 16383: a short search)
    static char twin[32];
    for (unsigned i = 0;; ++i) {
      snprintf(twin, sizeof twin, "x.t%u", i);
      if (fp(twin) == fp("sc.acid")) break;
    }
    static char ids[96];
    snprintf(ids, sizeof ids, "sc.acid %s sc.grain", twin);
    installed(ids);
    memset(&m, 0, sizeof m);
    CHECK(rmp_resolve_open(&m, &n, false, false));
    rmp_prints p = prints({{0, fp("sc.acid"), 3}});
    CHECK(rmp_resolve_prints(&m, &n, &p, pk, npk) == 0 && !m.slot[0].id[0] && (n.slot[0].flags & RMP_RF_COLLISION));
    // the exact id known (an imported list, or a resolve before): taken
    rmp_resolve_import(&m, &n, 0, twin, 0x010000, 0, "TWIN");
    memset(&m.slot[0], 0, sizeof m.slot[0]);   // (as if its map line went: only the note's wish remains)
    n.slot[0].src = RMP_SRC_NONE;
    CHECK(rmp_resolve_prints(&m, &n, &p, pk, npk) == 1u && !strcmp(m.slot[0].id, twin) && !(n.slot[0].flags & RMP_RF_COLLISION));
    installed("sc.acid sc.grain sc.metal");
  }

  // ---- explicit empty: stays, whatever the sounds say; inherit skips it too
  {
    memset(&m, 0, sizeof m);
    put(&m, 0, "sc.acid");
    CHECK(!rmp_resolve_open(&m, &n, true, false));
    rmp_resolve_set(&m, &n, 0, "", 0, RMP_SRC_SAVED);   // the user cleared U1
    for (int s = 1; s < RMP_SLOTS; ++s) rmp_resolve_set(&m, &n, s, nullptr, 0, RMP_SRC_SAVED);   // "start this project empty"
    rmp_prints p = prints({{0, fp("sc.acid"), 5}});
    CHECK(rmp_resolve_prints(&m, &n, &p, pk, npk) == 0 && !m.slot[0].id[0] && n.slot[0].src == RMP_SRC_EMPTY);
    CHECK(n.slot[0].fp == fp("sc.acid"));   // still noted for the UI
    CHECK(rmp_resolve_inherit(&m, &n, &prev) == 0);
    CHECK(!rmp_resolve_tag(&m, &n, 0, fp("sc.acid"), pk, npk));
    char txt[200];   // it survives the note file (the map has no line for it)
    rmp_slot_notes r;
    rmp_slot_map m2;
    memset(&m2, 0, sizeof m2);
    CHECK(rmp_notes_parse(txt, rmp_notes_format(&n, txt, sizeof txt), &r));
    CHECK(!rmp_resolve_open(&m2, &r, false, true) && r.slot[0].src == RMP_SRC_EMPTY && r.slot[4].src == RMP_SRC_EMPTY);
  }

  // ---- saved vs fingerprint conflict: the sounds win (UPDATED) unless any sound uses the saved machine
  {
    memset(&m, 0, sizeof m);
    put(&m, 0, "sc.metal"); put(&m, 1, "sc.metal"); put(&m, 2, "sc.metal");
    CHECK(!rmp_resolve_open(&m, &n, true, false));
    rmp_prints p = prints({{0, fp("sc.acid"), 9},
                           {1, fp("sc.acid"), 9}, {1, fp("sc.metal"), 1},   // one sound still uses the saved one
                           {2, 999, 4}});                                     // a machine not installed
    CHECK(rmp_resolve_prints(&m, &n, &p, pk, npk) == 1u);
    CHECK(!strcmp(m.slot[0].id, "sc.acid") && n.slot[0].src == RMP_SRC_SOUNDS && (n.slot[0].flags & RMP_RF_UPDATED));
    CHECK(!strcmp(m.slot[1].id, "sc.metal") && n.slot[1].src == RMP_SRC_SAVED && (n.slot[1].flags & RMP_RF_MIXED));
    CHECK(!strcmp(m.slot[2].id, "sc.metal") && (n.slot[2].flags & RMP_RF_UNKNOWN) && n.slot[2].fp == 999);   // kept
    // the same report again changes nothing
    CHECK(rmp_resolve_prints(&m, &n, &p, pk, npk) == 0);
    // a package with fingerprint 999 installed later: only the open slots resolve again (U1's update stays)
    CHECK(rmp_resolve_open_slots(&n) == 4u);
    static char nine[32];
    for (unsigned i = 0;; ++i) { snprintf(nine, sizeof nine, "y.p%u", i); if (fp(nine) == 999) break; }
    rmp_pkg_ref more[5];
    memcpy(more, pk, sizeof(rmp_pkg_ref) * (size_t)npk);
    more[npk] = rmp_pkg_ref{nine, 0x000100, 999, "NINE"};
    CHECK(rmp_resolve_prints(&m, &n, &p, more, npk + 1, rmp_resolve_open_slots(&n)) == 4u);
    CHECK(!strcmp(m.slot[2].id, nine) && (n.slot[2].flags & RMP_RF_UPDATED) && !(n.slot[2].flags & RMP_RF_UNKNOWN));
    CHECK(n.slot[0].src == RMP_SRC_SOUNDS && !strcmp(m.slot[0].id, "sc.acid") && rmp_resolve_open_slots(&n) == 0);
    // UPDATED is shown until the next load
    CHECK(!rmp_resolve_open(&m, &n, true, true) && !(n.slot[0].flags & RMP_RF_UPDATED));
  }

  // ---- an import of a machine not installed: MISSING by name; installing it later resolves the same id
  {
    memset(&m, 0, sizeof m);
    CHECK(rmp_resolve_open(&m, &n, false, false));
    rmp_resolve_import(&m, &n, 4, "ext.wobble", 0x000300, 0, "WOBBLE");
    CHECK(!strcmp(m.slot[4].id, "ext.wobble") && n.slot[4].src == RMP_SRC_IMPORTED && n.slot[4].fp == fp("ext.wobble"));
    CHECK(!strcmp(n.slot[4].name, "WOBBLE"));
    rmp_prints p = prints({{4, fp("ext.wobble"), 2}});
    CHECK(rmp_resolve_prints(&m, &n, &p, pk, npk) == 0 && n.slot[4].src == RMP_SRC_IMPORTED);   // the sounds agree
  }
  printf("resolve_test: all passed\n");
  return 0;
}
