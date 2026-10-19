// extras/presets_test.cpp - generic preset files, preset lists and the per-voice preset state, on the Mac.
// build: c++ -std=c++17 -O1 -Wall -I../src presets_test.cpp ../src/rmp_presets.cpp -o /tmp/presets_test && /tmp/presets_test
// (also with -g -fsanitize=address,undefined -fno-sanitize-recover=all)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "rmp_presets.h"
#include "rmp_format.h"
#include "rmp_manifest.h"

static float P[16];
static void report(float pre, float lev, float tone, float offs) {
  for (int k = 0; k < 16; ++k) P[k] = 0;
  P[0] = pre; P[1] = lev; P[2] = tone; P[3] = offs;
}
static rmp_preset_names NM;
static rmp_preset_index IX;
static bool add(const char *f, uint16_t dir, const char *ext) { return rmp_preset_add(&NM, &IX, f, dir, ext); }
static void none(int16_t e[16]) { for (int k = 0; k < 16; ++k) e[k] = -1; }

int main() {
  {   // the generic file: "P<n> <value>" lines; anything else ignored; a later line wins
    const char *t = "P1 10\nP2 200\nP16 127\r\n# comment\nP3 5 extra\nP0 1\nP17 3\n  P4   77  \nP2 20\np5 9\nP6\n";
    int16_t v[16];
    rmp_preset_parse(t, (uint32_t)strlen(t), v);
    CHECK(v[0] == 10 && v[1] == 20 && v[2] == -1 && v[3] == 77 && v[4] == 9 && v[5] == -1 && v[15] == 127);
    for (int k = 6; k < 15; ++k) CHECK(v[k] == -1);
    rmp_preset_parse("P1 10", 5, v);   // no final newline
    CHECK(v[0] == 10);
    // a UTF-8 BOM, a lone CR; no 4 digits, signs, decimals or '='; a tab is a blank
    const char *e = "\xEF\xBB\xBFP1 10\nP2 5\rP3 7\nP4 0127\nP5 -1\nP6 1.5\nP7=3\nP8 +4\nP9\t9\n";
    rmp_preset_parse(e, (uint32_t)strlen(e), v);
    CHECK(v[0] == 10 && v[1] == 5 && v[2] == 7 && v[8] == 9);
    for (int k = 3; k < 8; ++k) CHECK(v[k] == -1);
    rmp_preset_parse("P1 3\rP2 4", 9, v);
    CHECK(v[0] == 3 && v[1] == 4);
    // a truncated read: the cut last line is dropped (P16 127 cut to "P16 12"), a complete one kept
    const char *tr = "P1 5\nP16 127";
    rmp_preset_parse(tr, (uint32_t)strlen(tr) - 1, v, true);
    CHECK(v[0] == 5 && v[15] == -1);
    rmp_preset_parse(tr, (uint32_t)strlen(tr) - 1, v);   // without the flag: the caller's to drop
    CHECK(v[15] == 12);
    rmp_preset_parse("P1 5\n", 5, v, true);
    CHECK(v[0] == 5);
    rmp_preset_parse("P1 5\r", 5, v, true);
    CHECK(v[0] == 5);
    rmp_preset_parse("", 0, v);
    for (int k = 0; k < 16; ++k) CHECK(v[k] == -1);
    rmp_preset_parse(nullptr, 0, v);
    CHECK(v[0] == -1);
  }
  {   // the list: .txt generic, the manifest's extension native, name order, 127 at most, 13 characters
    rmp_preset_names_init(&NM);
    rmp_preset_index_init(&IX);
    rmp_preset_scan_begin(&NM, &IX, 0);
    CHECK(add("Lead.txt", 3, "syx"));
    CHECK(add("Bass.TXT", 4, "syx"));
    CHECK(add("Arp.SyX", 5, "syx"));
    CHECK(!add("Pad.wav", 6, "syx"));            // neither
    CHECK(!add("Pad.syx", 7, ""));               // no native presets declared
    CHECK(!add("Pad.syx", 7, nullptr));
    CHECK(!add(".txt", 8, "syx"));               // no name
    CHECK(!add("Bass.txt", 9, "syx"));           // a name already there, same kind: the first stays
    CHECK(add("AVeryLongPresetName.txt", 10, "syx"));
    CHECK(IX.n == 4);
    CHECK(!strcmp(NM.name[0], "AVeryLongPres") && !strcmp(NM.name[1], "Arp") && !strcmp(NM.name[2], "Bass") && !strcmp(NM.name[3], "Lead"));
    CHECK(IX.kind[1] == RMP_PRESET_NATIVE && IX.kind[2] == RMP_PRESET_GENERIC && IX.dir[1] == 5 && IX.dir[2] == 4);
    CHECK(add("Bass.syx", 11, "syx"));           // native replaces generic of its name
    CHECK(IX.n == 4 && IX.kind[2] == RMP_PRESET_NATIVE && IX.dir[2] == 11);
    CHECK(!add("Arp.txt", 12, "syx"));           // generic where a native one is: native stays
    CHECK(IX.kind[1] == RMP_PRESET_NATIVE && IX.dir[1] == 5);
    CHECK(!add("AVeryLongPresetName2.txt", 13, "syx"));   // cut to the same 13: the first stays
    CHECK(IX.dir[0] == 10);
    // dot files, blank names; byte order (case-sensitive); non-ASCII bytes '_'
    CHECK(!add("._Bass.txt", 14, "syx") && !add(".DS_Store.txt", 15, "syx") && !add("  .txt", 16, "syx"));
    CHECK(!add("noext", 17, "syx") && !add("", 17, "syx") && !add(nullptr, 17, "syx"));
    CHECK(add("zed.txt", 18, "") && add("abc.txt", 19, "") && add("Abc.txt", 20, "") && add("Caf\xC3\xA9.txt", 21, ""));
    CHECK(add("a b .txt", 22, ""));
    rmp_preset_scan_end(&NM, &IX);
    const char *want[] = {"AVeryLongPres", "Abc", "Arp", "Bass", "Caf__", "Lead", "a b ", "abc", "zed"};
    CHECK(IX.n == 9 && NM.n == 9);
    for (int i = 0; i < 9; ++i) CHECK(!strcmp(NM.name[i], want[i]));
    // 127 at most: the first by name
    rmp_preset_scan_begin(&NM, &IX, 0);
    char f[16];
    for (int i = 0; i < 130; ++i) { snprintf(f, sizeof f, "N%03d.txt", 129 - i); add(f, (uint16_t)i, ""); }
    rmp_preset_scan_end(&NM, &IX);
    CHECK(IX.n == 127 && NM.n == 127 && !strcmp(NM.name[0], "N000") && !strcmp(NM.name[126], "N126"));
    CHECK(IX.dir[0] == 129 && IX.dir[126] == 3);
    printf("sizes: names %zu B (one), index %zu B (a slot), rmp_pv %zu B (a voice), rmp_knobs %zu B\n",
           sizeof(rmp_preset_names), sizeof(rmp_preset_index), sizeof(rmp_pv), sizeof(rmp_knobs));
  }
  {   // generations: names cached by (slot, index generation); a changed list or an invalidate moves it on
    static rmp_preset_index ix0, ix1;
    rmp_preset_names_init(&NM);
    rmp_preset_index_init(&ix0); rmp_preset_index_init(&ix1);
    CHECK(!rmp_preset_index_ok(&ix0) && !rmp_preset_names_ok(&NM, &ix0, 0));
    rmp_preset_scan_begin(&NM, &ix0, 0);
    rmp_preset_add(&NM, &ix0, "A.txt", 1, ""); rmp_preset_add(&NM, &ix0, "B.txt", 2, "");
    rmp_preset_scan_end(&NM, &ix0);
    uint8_t g = ix0.gen;
    CHECK(g != 0 && rmp_preset_index_ok(&ix0) && rmp_preset_names_ok(&NM, &ix0, 0) && !rmp_preset_names_ok(&NM, &ix0, 1));
    rmp_preset_scan_begin(&NM, &ix0, 0);                     // the same list again: same generation
    rmp_preset_add(&NM, &ix0, "B.txt", 2, ""); rmp_preset_add(&NM, &ix0, "A.txt", 1, "");
    rmp_preset_scan_end(&NM, &ix0);
    CHECK(ix0.gen == g && rmp_preset_names_ok(&NM, &ix0, 0));
    rmp_preset_scan_begin(&NM, &ix1, 1);                     // another slot's scan takes the names
    rmp_preset_add(&NM, &ix1, "C.txt", 1, "");
    rmp_preset_scan_end(&NM, &ix1);
    CHECK(rmp_preset_names_ok(&NM, &ix1, 1) && !rmp_preset_names_ok(&NM, &ix0, 0));
    CHECK(rmp_preset_index_ok(&ix0) && ix0.n == 2 && ix0.dir[0] == 1 && ix0.dir[1] == 2);   // slot 0's index kept
    rmp_preset_scan_begin(&NM, &ix0, 0);                     // a different list: the generation moves on
    rmp_preset_add(&NM, &ix0, "A.txt", 1, "");
    rmp_preset_scan_end(&NM, &ix0);
    CHECK(ix0.gen != g && rmp_preset_names_ok(&NM, &ix0, 0));
    g = ix0.gen;
    rmp_preset_invalidate(&ix0);                             // reassigned: rescan, new generation
    CHECK(ix0.gen != g && ix0.gen != 0 && !rmp_preset_index_ok(&ix0) && !rmp_preset_names_ok(&NM, &ix0, 0));
    ix0.gen = 255; rmp_preset_invalidate(&ix0); CHECK(ix0.gen == 1);   // 0 is never a generation
  }
  {   // rmp_pv: knobs P1 PRE (kind preset), P2 LEV and P3 TONE role preset, P4 OFFS role offset
    uint8_t roles[16] = {0};
    roles[1] = roles[2] = RMP_ROLE_PRESET; roles[3] = RMP_ROLE_OFFSET;
    const int pre = 0, total = 3;
    rmp_pv v; rmp_pv_reset(&v);
    bool sync;
    uint8_t ok[16], ov[16];
    // the voice first becomes this machine (a knob report): the sound's entry, written back
    report(1, 50, 50, 64);
    CHECK(rmp_pv_decide(&v, pre, total, false, true, P, &sync) == 1 && sync);
    int16_t e1[16]; none(e1);
    e1[1] = 100; e1[2] = 20; e1[3] = 99;   // P4 is an offset knob: never written back
    CHECK(rmp_pv_loaded(&v, 1, true, e1, roles, 1000, P, ok, ov) == 2);
    CHECK(ok[0] == 1 && ov[0] == 100 && ok[1] == 2 && ov[1] == 20);   // 1C: knobs P2, P3
    // the hold: reports still carrying the old values play the preset's
    report(1, 50, 50, 64); rmp_pv_apply(&v, P, false, 1100);
    CHECK(P[1] == 100 && P[2] == 20 && P[3] == 64);
    // ... until a report carries them all (the Rytm wrote them)
    report(1, 100, 20, 64); rmp_pv_apply(&v, P, false, 1150);
    CHECK(v.hold == 0);
    report(1, 90, 20, 64); rmp_pv_apply(&v, P, false, 1160);   // then knob turns play
    CHECK(P[1] == 90);
    // or until 300 ms passed
    report(2, 0, 0, 64);
    CHECK(rmp_pv_decide(&v, pre, total, false, false, P, &sync) == 2 && sync);   // PRE turned (0x03)
    int16_t e2[16]; none(e2);
    e2[1] = 30;
    CHECK(rmp_pv_loaded(&v, 2, true, e2, roles, 2000, P, ok, ov) == 1 && ok[0] == 1 && ov[0] == 30);
    report(2, 0, 0, 64); rmp_pv_apply(&v, P, false, 2299); CHECK(P[1] == 30);
    report(2, 0, 0, 64); rmp_pv_apply(&v, P, false, 2300); CHECK(P[1] == 0 && v.hold == 0);
    // the same PRE again (a 0x03 for another knob): nothing to load
    report(2, 5, 0, 64);
    CHECK(rmp_pv_decide(&v, pre, total, false, false, P, &sync) == -1);
    // PRE beyond the list plays the last entry; huge or NaN PRE are clamped (no UB)
    report(120, 30, 0, 64);
    CHECK(rmp_pv_decide(&v, pre, total, false, false, P, &sync) == -1);   // min(120, 2) = 2 = synced
    report(1e30f, 30, 0, 64);
    CHECK(rmp_pv_decide(&v, pre, total, false, false, P, &sync) == -1);
    report(-1e30f, 30, 0, 64);
    CHECK(rmp_pv_decide(&v, pre, total, false, false, P, &sync) == 0 && sync);   // entry 0 (not loaded here)
    report(NAN, 30, 0, 64);
    CHECK(rmp_pv_decide(&v, pre, total, false, false, P, &sync) == 0 && sync);
    report(2, 30, 0, 64);
    CHECK(rmp_pv_decide(&v, pre, total, false, false, P, &sync) == -1);   // back on 2 = synced
    // a p-locked PRE on a trig: play entry 0 without write-back, override P2/P3 until a knob moves
    report(0, 30, 0, 64);
    CHECK(rmp_pv_decide(&v, pre, total, true, false, P, &sync) == 0 && !sync);
    int16_t e0[16]; none(e0);
    e0[1] = 77; e0[2] = 66;
    CHECK(rmp_pv_loaded(&v, 0, false, e0, roles, 3000, P, ok, ov) == 0 && v.plock);   // no 1C
    rmp_pv_apply(&v, P, true, 3000); CHECK(P[1] == 77 && P[2] == 66);
    report(2, 30, 0, 64); rmp_pv_apply(&v, P, false, 3100); CHECK(P[1] == 77 && P[2] == 66);   // unchanged knobs: still the p-lock's
    report(2, 31, 0, 64); rmp_pv_apply(&v, P, false, 3200); CHECK(P[1] == 31 && P[2] == 66);   // P2 turned: the turn wins
    // the same p-locked PRE on the next trig: nothing new to load; the override starts again
    report(0, 31, 0, 64);
    CHECK(rmp_pv_decide(&v, pre, total, true, false, P, &sync) == -1);
    rmp_pv_apply(&v, P, true, 3250); CHECK(P[1] == 77 && P[2] == 66);
    // a knob p-locked on the same trig as PRE keeps its own lock (the sound has P3 0, the step 40)
    report(0, 31, 40, 64); rmp_pv_apply(&v, P, true, 3260); CHECK(P[1] == 77 && P[2] == 40);
    // a trig without the p-lock: back to the sound's own entry (loaded again, no write-back, no override)
    report(2, 31, 0, 64);
    CHECK(rmp_pv_decide(&v, pre, total, true, false, P, &sync) == 2 && !sync);
    CHECK(rmp_pv_loaded(&v, 2, false, e2, roles, 3300, P, ok, ov) == 0 && !v.plock && v.mask == 0);
    rmp_pv_apply(&v, P, true, 3300); CHECK(P[1] == 31);
    // no preset knob, or no presets: nothing
    CHECK(rmp_pv_decide(&v, -1, total, true, true, P, &sync) == -1);
    CHECK(rmp_pv_decide(&v, pre, 0, true, true, P, &sync) == -1);
    CHECK(rmp_pv_decide(&v, 16, total, false, true, P, &sync) == -1);
  }
  {   // the first report a trig: played without write-back; the next knob report syncs
    uint8_t roles[16] = {0}; roles[1] = RMP_ROLE_PRESET;
    rmp_pv v; rmp_pv_reset(&v);
    bool sync; uint8_t ok[16], ov[16];
    int16_t e[16]; none(e); e[1] = 120;
    report(1, 10, 0, 64);
    CHECK(rmp_pv_decide(&v, 0, 3, true, true, P, &sync) == 1 && !sync);
    CHECK(rmp_pv_loaded(&v, 1, false, e, roles, 0, P, ok, ov) == 0);
    rmp_pv_apply(&v, P, true, 0); CHECK(P[1] == 120);
    report(1, 10, 0, 64);
    CHECK(rmp_pv_decide(&v, 0, 3, false, false, P, &sync) == 1 && sync);
    CHECK(rmp_pv_loaded(&v, 1, true, e, roles, 10, P, ok, ov) == 1 && ov[0] == 120 && !v.plock);
  }
  {   // a list generation change forgets the entries; a failed load is not asked again
    uint8_t roles[16] = {0}; roles[1] = RMP_ROLE_PRESET;
    rmp_pv v; rmp_pv_reset(&v);
    bool sync; uint8_t ok[16], ov[16];
    int16_t e[16]; none(e); e[1] = 5;
    rmp_pv_check(&v, 2);
    report(1, 0, 0, 64);
    CHECK(rmp_pv_decide(&v, 0, 3, false, true, P, &sync) == 1 && sync);
    rmp_pv_loaded(&v, 1, true, e, roles, 0, P, ok, ov);
    CHECK(rmp_pv_decide(&v, 0, 3, false, false, P, &sync) == -1);
    rmp_pv_check(&v, 2);
    CHECK(v.synced == 1);                                        // same generation: kept
    rmp_pv_check(&v, 3);                                         // rescanned / reassigned
    CHECK(v.synced == -1 && v.playing == -1 && v.hold == 0);
    CHECK(rmp_pv_decide(&v, 0, 3, false, false, P, &sync) == 1 && sync);   // reloaded
    rmp_pv_failed(&v, 1);
    CHECK(rmp_pv_decide(&v, 0, 3, false, false, P, &sync) == -1);          // not asked again
    report(1, 9, 0, 64);
    CHECK(rmp_pv_decide(&v, 0, 3, true, false, P, &sync) == -1);           // nor on a trig
    report(0, 0, 0, 64);
    CHECK(rmp_pv_decide(&v, 0, 3, false, false, P, &sync) == 0 && sync);   // another entry
    rmp_pv_loaded(&v, 0, true, e, roles, 0, P, ok, ov);
    report(1, 5, 0, 64);
    CHECK(rmp_pv_decide(&v, 0, 3, false, false, P, &sync) == 1 && sync);   // back: asked again
    rmp_pv_invalidate(&v);
    CHECK(v.synced == -1 && v.failed == -1);
  }
  {   // knob ranges from the description: preset values clamped before 1C; offsets on top of the entry's
    static rmp_desc d;
    memset(&d, 0, sizeof d);
    d.nknobs = 4;
    d.knob[0].p = 1; d.knob[0].role = RMP_ROLE_FREE;   d.knob[0].min = 0;  d.knob[0].max = 127;
    d.knob[1].p = 2; d.knob[1].role = RMP_ROLE_PRESET; d.knob[1].min = 10; d.knob[1].max = 80;
    d.knob[2].p = 4; d.knob[2].role = RMP_ROLE_OFFSET; d.knob[2].min = 10; d.knob[2].max = 100;
    d.knob[3].p = 17; d.knob[3].role = RMP_ROLE_PRESET;   // out of 1..16: ignored
    rmp_knobs r;
    rmp_knobs_from_desc(&r, &d);
    CHECK(r.role[1] == RMP_ROLE_PRESET && r.min[1] == 10 && r.max[1] == 80 && r.role[3] == RMP_ROLE_OFFSET);
    CHECK(r.role[2] == RMP_ROLE_FREE && r.min[2] == 0 && r.max[2] == 127);
    rmp_pv v; rmp_pv_reset(&v);
    bool sync; uint8_t ok[16], ov[16];
    int16_t e[16]; none(e); e[1] = 100; e[2] = 50; e[3] = 99;   // P3 is free: never touched
    report(1, 0, 7, 64);
    // before any preset: offsets untouched
    rmp_pv_effective(&v, &r, P); CHECK(P[3] == 64);
    CHECK(rmp_pv_decide(&v, 0, 3, false, true, P, &sync) == 1 && sync);
    CHECK(rmp_pv_load(&v, 1, true, e, &r, 0, P, ok, ov) == 1 && ok[0] == 1 && ov[0] == 80);   // clamped to max
    report(1, 0, 7, 64); rmp_pv_apply(&v, P, false, 10); rmp_pv_effective(&v, &r, P);
    CHECK(P[1] == 80 && P[2] == 7 && P[3] == 99);     // held; free untouched; offset 64 plays the entry's 99
    report(1, 80, 7, 70); rmp_pv_apply(&v, P, false, 20); rmp_pv_effective(&v, &r, P);
    CHECK(v.hold == 0 && P[1] == 80 && P[3] == 100);   // 99 + 6 = 105, clamped to 100
    report(1, 80, 7, 0); rmp_pv_effective(&v, &r, P); CHECK(P[3] == 35);   // 99 - 64
    d.knob[1].min = 90; rmp_knobs_from_desc(&r, &d);
    rmp_pv_load(&v, 1, true, e, &r, 30, P, ok, ov); CHECK(ov[0] == 100);   // a broken range: 0..127
    e[1] = 0; d.knob[1].min = 10; rmp_knobs_from_desc(&r, &d);
    rmp_pv_load(&v, 1, true, e, &r, 30, P, ok, ov); CHECK(ov[0] == 10);    // clamped to min
    // during the hold, a knob p-locked on a trig keeps its lock
    report(1, 0, 7, 64); rmp_pv_apply(&v, P, false, 40); CHECK(P[1] == 10);
    report(1, 55, 7, 64); rmp_pv_apply(&v, P, true, 50); CHECK(P[1] == 55);
    report(1, 0, 7, 64); rmp_pv_apply(&v, P, false, 60); CHECK(P[1] == 10);
    // huge report values: clamped, no UB
    report(1, 1e30f, NAN, -1e30f); rmp_pv_apply(&v, P, true, 70); rmp_pv_effective(&v, &r, P);
    CHECK(P[3] == 10);   // 99 + (-1e30 - 64): the minimum
    rmp_knobs_from_desc(&r, nullptr); CHECK(r.role[1] == RMP_ROLE_FREE && r.max[1] == 127);
  }
  printf("ok\n");
  return 0;
}
