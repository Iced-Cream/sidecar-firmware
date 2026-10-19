// extras/proto_test.cpp - the user-slot messages 19..1E (sim/protocol.py) on the Mac.
// build: c++ -std=c++17 -O1 -Wall -I../src proto_test.cpp ../src/rmp_proto.cpp ../src/rmp_manifest.cpp ../src/rmp_loader.cpp -o /tmp/proto_test && /tmp/proto_test
// -DPROTO_DUMP: print the fixed messages below as hex lines instead (sim/tests/test_protocol_us.py compares Python's)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "rmp_proto.h"

static void knob(rmp_desc *d, int p, int look, int kind, int role, int lo, int hi, int def, int ne, const char *l, const char *s) {
  rmp_desc_knob &k = d->knob[d->nknobs++];
  memset(&k, 0, sizeof k);
  k.p = (uint8_t)p; k.look = (uint8_t)look; k.kind = (uint8_t)kind; k.role = (uint8_t)role;
  k.min = (uint8_t)lo; k.max = (uint8_t)hi; k.def = (uint8_t)def; k.enum_count = (uint8_t)ne;
  strncpy(k.long_name, l, 12); strncpy(k.short_name, s, 4);
}
// The description every dump and test uses: 5 knobs, so two knob parts (4 + 1).
static void desc(rmp_desc *d) {
  memset(d, 0, sizeof *d);
  d->pre = 1;
  memcpy(d->name, "NP CLUST1", 9); memcpy(d->short_name, "NPC1", 4); memcpy(d->p2_title, "PAGE TWO", 8);
  knob(d, 1, 3, 2, 1, 0, 127, 0, 4, "Algorithm", "ALG");
  knob(d, 2, 0, 6, 0, 0, 127, 0, 0, "Preset", "PRE");
  knob(d, 3, 2, 0, 2, 10, 120, 100, 0, "Level", "LEV");
  knob(d, 5, 0, 7, 0, 0, 127, 0, 0, "Sample", "SMP");
  knob(d, 9, 1, 1, 0, 0, 127, 64, 0, "Tune\x01", "TUN");
}
#ifdef PROTO_DUMP
static void hex(const uint8_t *m, int n) { for (int i = 0; i < n; ++i) printf("%02x", m[i]); printf("\n"); }
#endif

int main() {
  rmp_desc d; desc(&d);
  uint8_t m[RMP_MSG_MAX];
  char names[8][14];
  for (int i = 0; i < 8; ++i) snprintf(names[i], sizeof names[i], "PRE%d", i);
  uint8_t gens[5] = {1, 2, 3, 4, 5};
  uint8_t kk[2] = {1, 2}, kv[2] = {64, 100};
#ifdef PROTO_DUMP
  int n;
  n = rmp_build_meta_unchanged(m, 2, 5); hex(m, n);
  n = rmp_build_meta_head(m, 1, 3, RMP_PST_READY, RMP_FLAG_PARAMS | RMP_FLAG_PRESETS, &d, rmp_plugin_fp("rytm.v2_test")); hex(m, n);
  n = rmp_build_meta_part(m, 1, 3, 1, &d); hex(m, n);
  n = rmp_build_meta_part(m, 1, 3, 2, &d); hex(m, n);
  n = rmp_build_meta_head(m, 4, 9, RMP_PST_EMPTY, 0, nullptr, 0); hex(m, n);
  n = rmp_build_list(m, 1, RMP_LIST_PRESETS, 3, 1, 10, 0, 8, names[0], 14); hex(m, n);
  n = rmp_build_knobs(m, 3, 2, 2, kk, kv); hex(m, n);
  n = rmp_build_slots(m, gens, 7, 0, 0); hex(m, n);
  n = rmp_build_slots(m, gens, 7, 2, RMP_RES_NO_ROOM); hex(m, n);
  return 0;
#endif
  // 1A unchanged: 10 bytes
  CHECK(rmp_build_meta_unchanged(m, 2, 5) == 10);
  { const uint8_t w[10] = {0xF0, 0x7D, 0x52, 0x54, 0x1A, 1, 2, 5, 0x7F, 0xF7}; CHECK(!memcmp(m, w, 10)); }
  // the sound tag's fingerprint: the vectors shared with Python (sim/tests/test_protocol_us.py SoundTag.FP_VECTORS)
  CHECK(rmp_plugin_fp("") == 8768 && rmp_plugin_fp("a") == 14699 && rmp_plugin_fp("rytm.v2_test") == 12437);
  CHECK(rmp_plugin_fp("befaco.np.atari") == 3424 && rmp_plugin_fp("befaco.np.a") == 11572 && rmp_plugin_fp("RYTM.V2_TEST") == 8663);
  // 1A part 0: state, flags, knob count, part count, name 10, short 4, page-2 title 13, fp 2 = 43 bytes
  CHECK(rmp_meta_nparts(&d) == 3 && rmp_meta_nparts(nullptr) == 1);
  CHECK(rmp_build_meta_head(m, 1, 3, RMP_PST_READY, 5, &d, 12437) == 43);
  CHECK(m[4] == 0x1A && m[5] == 1 && m[6] == 1 && m[7] == 3 && m[8] == 0 && m[9] == 1 && m[10] == 5 && m[11] == 5 && m[12] == 3);
  CHECK(!memcmp(m + 13, "NP CLUST1\0", 10) && !memcmp(m + 23, "NPC1", 4) && !memcmp(m + 27, "PAGE TWO\0\0\0\0\0", 13) && m[42] == 0xF7);
  CHECK(m[40] == (12437 >> 7) && m[41] == (12437 & 0x7F));
  for (int st = RMP_PST_EMPTY; st <= RMP_PST_MUTED; ++st)   // only READY carries the fingerprint
    CHECK(rmp_build_meta_head(m, 1, 3, st, 0, &d, 0x3FFF) == 43 && m[40] == (st == RMP_PST_READY ? 0x7F : 0) &&
          m[41] == (st == RMP_PST_READY ? 0x7F : 0));
  // knob parts: 9 header bytes, 24 a knob, F7
  CHECK(rmp_build_meta_part(m, 1, 3, 1, &d) == 9 + 4 * 24 + 1);
  CHECK(m[8] == 1 && m[9] == 1 && m[10] == 3 && m[11] == 2 && m[12] == 1 && m[13] == 0 && m[14] == 127 && m[15] == 0 && m[16] == 4);
  CHECK(!memcmp(m + 17, "Algorithm\0\0\0", 12) && !memcmp(m + 29, "ALG\0", 4));
  CHECK(m[9 + 2 * 24] == 3 && m[9 + 2 * 24 + 4] == 10 && m[9 + 2 * 24 + 5] == 120 && m[9 + 2 * 24 + 6] == 100);
  CHECK(rmp_build_meta_part(m, 1, 3, 2, &d) == 9 + 24 + 1);
  CHECK(m[9] == 9 && !memcmp(m + 17, "Tune_\0", 6));   // the control character became '_'
  CHECK(rmp_build_meta_part(m, 1, 3, 3, &d) == 0 && rmp_build_meta_part(m, 1, 3, 0, &d) == 0);   // no such part
  // 1026: the unused knobs padded as empty labels (P4 P6 P7 P8; page2 0: not P9..P16), in p order, pre moved along
  {
    rmp_desc u = d;
    CHECK(rmp_desc_pad_unused(&u) == 4 && u.nknobs == 9 && rmp_meta_nparts(&u) == 4);
    for (int k = 0; k < 8; ++k) CHECK(u.knob[k].p == k + 1);
    CHECK(u.knob[8].p == 9 && u.pre == 1 && !u.knob[3].long_name[0] && !u.knob[3].short_name[0] && u.knob[3].max == 0);
    CHECK(!strcmp(u.knob[4].long_name, "Sample"));
    rmp_desc f;   // a machine with 5 params on both pages: P6..P16 unused
    memset(&f, 0, sizeof f);
    f.page2 = 1, f.pre = -1;
    for (int p = 1; p <= 5; ++p) knob(&f, p, 0, 0, 0, 0, 127, 0, 0, "Knob", "KNB");
    CHECK(rmp_desc_pad_unused(&f) == 11 && f.nknobs == 16 && rmp_meta_nparts(&f) == 5);
    CHECK(rmp_build_meta_part(m, 1, 3, 2, &f) == 9 + 4 * 24 + 1 && m[9] == 5 && !memcmp(m + 17, "Knob", 4));
    CHECK(m[9 + 24] == 6 && !m[9 + 24 + 8] && !m[9 + 24 + 20]);   // P6: empty long and short labels
    CHECK(rmp_build_meta_part(m, 1, 3, 4, &f) == 9 + 4 * 24 + 1 && m[9 + 3 * 24] == 16 && !m[9 + 3 * 24 + 8]);
  }
  // EMPTY: no names, no knobs, one part
  CHECK(rmp_build_meta_head(m, 4, 9, RMP_PST_EMPTY, 0, nullptr, 0) == 43 && m[11] == 0 && m[12] == 1 && m[13] == 0);
  // 1B: 11 header bytes, 14 an entry, F7: 8 entries = 124 bytes
  CHECK(rmp_build_list(m, 1, RMP_LIST_PRESETS, 3, 1, 10, 0, 8, names[0], 14) == 124);
  CHECK(m[4] == 0x1B && m[6] == 1 && m[7] == 0x20 && m[8] == 3 && m[9] == 1 && m[10] == 10 && m[11] == 0 && !memcmp(m + 12, "PRE0\0", 5));
  CHECK(m[11 + 7 * 14] == 7 && m[123] == 0xF7);
  CHECK(rmp_build_list(m, 0, RMP_LIST_INSTALLED, 2, 0, 0, 0, 0, names[0], 14) == 12);   // an empty list: one message
  CHECK(rmp_build_list(m, 0x81, 0xA0, 3, 1, -1, 0, -2, names[0], 14) == 12 && m[6] == 1 && m[7] == 0x20 && m[10] == 0);   // total -1: 0
  CHECK(rmp_build_list(m, 1, RMP_LIST_PRESETS, 3, 1, 200, 0, 1, names[0], 14) == 26 && m[10] == 127);                   // total 200: 127
  // 1C: voice, slot, n, pairs
  CHECK(rmp_build_knobs(m, 3, 2, 2, kk, kv) == 9 + 4 + 1);
  { const uint8_t w[14] = {0xF0, 0x7D, 0x52, 0x54, 0x1C, 1, 3, 2, 2, 1, 64, 2, 100, 0xF7}; CHECK(!memcmp(m, w, 14)); }
  CHECK(rmp_build_knobs(m, 3, 0x82, -1, kk, kv) == 10 && m[7] == 2 && m[8] == 0 && m[9] == 0xF7);   // n -1: none
  // 1D: five generations and the installed list's, an optional answer pair
  CHECK(rmp_build_slots(m, gens, 7, 0, 0) == 13);
  { const uint8_t w[13] = {0xF0, 0x7D, 0x52, 0x54, 0x1D, 1, 1, 2, 3, 4, 5, 7, 0xF7}; CHECK(!memcmp(m, w, 13)); }
  CHECK(rmp_build_slots(m, gens, 7, 2, RMP_RES_NO_ROOM) == 15 && m[12] == 2 && m[13] == 2 && m[14] == 0xF7);
  CHECK(rmp_build_slots(m, gens, 7, 0x83, 0x85) == 15 && m[12] == 3 && m[13] == 5);
  CHECK(rmp_build_meta_head(m, 0x81, 3, 0x86, 0, nullptr, 0) == 43 && m[6] == 1 && m[9] == 6);
  for (int i = 0; i < 43; ++i) CHECK(i == 0 || i == 42 || m[i] < 0x80);
  CHECK(rmp_build_meta_head(m, 1, 3, RMP_PST_READY, 0, nullptr, 0xFFFF) == 43 && m[40] == 0x7F && m[41] == 0x7F);   // 7-bit bytes
  // the reason in a non-READY slot's name: "<TAG> <short>" in at most 9 characters (the Rytm shows "!" + it)
  { char w[10];
    rmp_desc q; memset(&q, 0, sizeof q); strcpy(q.name, "NP PRIME"); strcpy(q.short_name, "NPPR");
    rmp_why_name(w, RMP_WHY_ROOM, &q); CHECK(!strcmp(w, "ROOM NPPR"));
    rmp_why_name(w, RMP_WHY_BAD, &q); CHECK(!strcmp(w, "BAD NPPR"));
    rmp_why_name(w, RMP_WHY_NONE, &q); CHECK(!w[0]);
    rmp_why_name(w, RMP_WHY_MISS, nullptr); CHECK(!strcmp(w, "MISS"));
    q.short_name[0] = 0; rmp_why_name(w, RMP_WHY_TIME, &q); CHECK(!strcmp(w, "TIME NP P"));
    CHECK(rmp_why_of(RMP_PST_NO_ROOM, 0) == RMP_WHY_ROOM && rmp_why_of(RMP_PST_MISSING, 0) == RMP_WHY_MISS &&
          rmp_why_of(RMP_PST_BAD, 0) == RMP_WHY_BAD && rmp_why_of(RMP_PST_MUTED, 0) == RMP_WHY_CRASH &&
          rmp_why_of(RMP_PST_MUTED, RMP_WHY_TIME) == RMP_WHY_TIME && rmp_why_of(RMP_PST_READY, RMP_WHY_TIME) == RMP_WHY_NONE &&
          rmp_why_of(RMP_PST_LOADING, 0) == RMP_WHY_NONE);
    strcpy(q.short_name, "NPPR");
    CHECK(rmp_build_meta_head_why(m, 1, 3, RMP_PST_MUTED, 0, &q, 0, RMP_WHY_CRASH) == 43 && !memcmp(m + 13, "CRSH NPPR\0", 10) &&
          !memcmp(m + 23, "NPPR", 4));
    uint8_t m2[RMP_MSG_MAX];   // NONE: the same message as rmp_build_meta_head
    CHECK(rmp_build_meta_head_why(m2, 1, 3, RMP_PST_READY, 0, &q, 7, RMP_WHY_NONE) == 43 &&
          rmp_build_meta_head(m, 1, 3, RMP_PST_READY, 0, &q, 7) == 43 && !memcmp(m, m2, 43)); }
  // 19 ASK: parsed; another version, command or length is not
  rmp_ask a;
  { const uint8_t q[11] = {0xF0, 0x7D, 0x52, 0x54, 0x19, 1, 3, 7, RMP_ASK_ENUM, 2, 0xF7};
    CHECK(rmp_parse_ask(q, 11, &a) && a.slot == 3 && a.gen == 7 && a.what == RMP_ASK_ENUM && a.arg == 2);
    uint8_t v[11]; memcpy(v, q, 11); v[5] = 2; CHECK(!rmp_parse_ask(v, 11, &a));        // version 2: ignored
    memcpy(v, q, 11); v[4] = 0x1E; CHECK(!rmp_parse_ask(v, 11, &a));
    memcpy(v, q, 11); v[6] = 6; CHECK(!rmp_parse_ask(v, 11, &a));                       // slot 6
    memcpy(v, q, 11); v[8] = 4; CHECK(!rmp_parse_ask(v, 11, &a));                       // what 4
    CHECK(!rmp_parse_ask(q, 10, &a));
    uint8_t big[12]; memcpy(big, q, 10); big[10] = 0; big[11] = 0xF7; CHECK(!rmp_parse_ask(big, 12, &a));   // 12 bytes
    memcpy(v, q, 11); v[6] = 0; CHECK(!rmp_parse_ask(v, 11, &a));                       // ENUM with slot 0
    memcpy(v, q, 11); v[8] = RMP_ASK_INSTALLED; CHECK(!rmp_parse_ask(v, 11, &a)); }     // INSTALLED with slot 3
  { const uint8_t q[11] = {0xF0, 0x7D, 0x52, 0x54, 0x19, 1, 0, 0, RMP_ASK_INSTALLED, 0, 0xF7};
    CHECK(rmp_parse_ask(q, 11, &a) && a.slot == 0 && a.what == RMP_ASK_INSTALLED); }
  // 1E ASSIGN
  rmp_assign s;
  { const uint8_t q[11] = {0xF0, 0x7D, 0x52, 0x54, 0x1E, 1, 5, RMP_OP_INSTALL, 0, 9, 0xF7};
    CHECK(rmp_parse_assign(q, 11, &s) && s.slot == 5 && s.op == RMP_OP_INSTALL && s.arg == 0 && s.installed_gen == 9);
    uint8_t v[11]; memcpy(v, q, 11); v[6] = 0; CHECK(!rmp_parse_assign(v, 11, &s));    // slot 0
    memcpy(v, q, 11); v[7] = 3; CHECK(!rmp_parse_assign(v, 11, &s));                   // op 3
    memcpy(v, q, 11); v[5] = 0; CHECK(!rmp_parse_assign(v, 11, &s)); }                 // version 0
  // slot states: rmp_slot_state's numbering -> the protocol's
  CHECK(rmp_proto_state(RMP_SLOT_EMPTY) == RMP_PST_EMPTY && rmp_proto_state(RMP_SLOT_READY) == RMP_PST_READY);
  CHECK(rmp_proto_state(RMP_SLOT_LOADING) == RMP_PST_LOADING && rmp_proto_state(RMP_SLOT_MISSING) == RMP_PST_MISSING);
  CHECK(rmp_proto_state(RMP_SLOT_BAD) == RMP_PST_BAD && rmp_proto_state(RMP_SLOT_NO_ROOM) == RMP_PST_NO_ROOM);
  CHECK(rmp_proto_state(RMP_SLOT_MUTED) == RMP_PST_MUTED);
  printf("ok\n");
  return 0;
}
