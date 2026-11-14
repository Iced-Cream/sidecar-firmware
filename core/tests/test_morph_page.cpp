// core/tests/test_morph_page.cpp - the MORPH page (core/ui/page_morph.cpp, spec-050 2.6) through the remote host, as
// test_lfo.cpp's page part: the scene keys (tap selects, T15 / T16 held + Tn A / B, Tn held its lock list), the fader
// on encoder A, the list's edits (B..H, LEFT / RIGHT, NO removes, YES twice clears), no SCN / PERF cells (v2),
// the ADD row (v2 Task 5), the trig LEDs, and every text on the Rytm's screen fits (check.h's TEXT-FIT); on the other
// Sidecar pages the momentary EDIT (v2): PAGE + T15 / T16 held = scene A / B, a turn locks into it, + T1..T12 picks it.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "machines.h"
#include "voices.h"
#include "rui_png.h"
#include "core.h"
#include "mod/mod.h"
#include "mod/scene.h"
#include "mod/target.h"
#include "ui/extra.h"
#include "ui/remote.h"

using namespace core;
using mod::Tid;

static uint64_t g_us = 1000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static int g_mask_encs = -1, g_masks = 0;
static uint32_t g_mask_trigs = 0;
static bool g_mask_func = false;
static uint8_t g_led[16];
static bool g_leds_on = false;
static int g_closes = 0;   // 3A 0 0: the host asks the page closed (nav044: NO on a shortcut's page)
static void out(const uint8_t* d, int len) {
  if (len < 7 || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54) return;
  if (d[4] == 0x37 && len >= 10) {
    g_mask_encs = d[6] | d[7] << 7 | d[8] << 14, g_mask_trigs = 0, ++g_masks;
    const uint8_t* k = d + 9;
    int nk = len - 10;
    for (int c = 0; c < 16; ++c)
      if (c / 7 < nk && (k[c / 7] >> (c % 7) & 1)) g_mask_trigs |= 1u << c;
    g_mask_func = 0x80 / 7 < nk && (k[0x80 / 7] >> (0x80 % 7) & 1);
  } else if (d[4] == 0x3A && len >= 9 && d[7] == 0) {
    ++g_closes;
  } else if (d[4] == 0x34) {
    g_leds_on = len >= 24 && d[6] == 1;
    if (g_leds_on) memcpy(g_led, d + 7, 16);
  }
}
static void from_rytm(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c) {
  const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, a, b, c, 0xF7};
  engine_sysex(m, 10);
}
static void tick(int n = 4) {
  for (int i = 0; i < n; ++i) at(g_us + 30000), ui::remote_tick(now_ms());
}
static void down(int code) { from_rytm(0x31, (uint8_t)(code >> 7), code & 0x7F, 1), tick(1); }
static void up(int code) { from_rytm(0x31, (uint8_t)(code >> 7), code & 0x7F, 0), tick(1); }
static void tap(int code) { down(code), up(code); }
static void turn(int e, int delta, bool func = false) { from_rytm(0x32, (uint8_t)e, (uint8_t)(64 + delta), func ? 1 : 0), tick(1); }
static Tid T(const char* n) {
  Tid t = 0;
  CHECK(mod::parse(n, &t));
  return t;
}
static int lock_v(int n, Tid t) {
  const scene::Scene& s = scene::state().s[n];
  for (int i = 0; i < s.n; ++i)
    if (s.l[i].t == t) return s.l[i].v;
  return -1;
}
// the page's frame as the Rytm would get it: the text items
static ui::Canvas g_c;
static void draw() { ui::kMorphPage.draw(g_c, 0, 1, now_ms() | 1); }
static void png(const char* name) {   // the frame as the Rytm shows it (PNGs to $RUI_OUT, default build/rui_morph)
  static std::string dir = rui_png_dir("RUI_OUT", "build/rui_morph");
  uint8_t fb[ui::kBytes];
  draw();
  ui::preview(g_c, fb);
  rui_png(dir, name, fb);
}
static bool shows(const char* s) {
  draw();
  for (const ui::TextItem& t : g_c.txt)
    if (t.s[0] && strstr(t.s, s)) return true;
  return false;
}

static void open_page() {
  ui::remote_reset();
  ui::morph_page_reset();
  scene::reset();
  from_rytm(0x30, 0, 1, 0);
  tick();
  ui::remote_show_page("MORPH");
  tick();
  CHECK(ui::remote_page_title() && !strcmp(ui::remote_page_title(), "MORPH"));
}

static void select_and_ab_by_holds() {
  open_page();
  CHECK(g_mask_trigs == 0xFFFF && g_mask_encs == 0xFF && g_mask_func);   // all 16 trig keys, A..H, FUNC (both sides')
  tap(0x02);
  CHECK(scene::state().sel == 2);
  scene::set_fader(5000, scene::kConsole);
  down(0x0E), tap(0x04), up(0x0E);   // T15 held + T5: A = 5
  CHECK(scene::state().a == 4 && scene::state().b == -1 && scene::state().sel == 2 && scene::state().fader == 5000);
  down(0x0F), tap(0x06), up(0x0F);   // T16 held + T7: B = 7
  CHECK(scene::state().a == 4 && scene::state().b == 6 && scene::state().fader == 5000);
  tap(0x0C), tap(0x0D);              // T13 / T14: nothing
  CHECK(scene::state().sel == 2 && ui::remote_page_title() && !strcmp(ui::remote_page_title(), "MORPH"));
  tap(0x41), tap(0x41);              // RIGHT: the next scene (the page stays)
  CHECK(scene::state().sel == 4 && !strcmp(ui::remote_page_title(), "MORPH"));
  tap(0x40);
  CHECK(scene::state().sel == 3);
  CHECK(scene::lock(2, T("V1:P3"), 6400) && scene::lock(6, T("V2:P1"), 640));
  scene::set_fader(8192, scene::kConsole);
  CHECK(shows("A5") && shows("B7") && shows("MORPH"));
  png("morph_main.png");
  printf("  select_and_ab_by_holds: T3 selects, T15 + T5 A, T16 + T7 B, the fader stays; LEFT / RIGHT the scene\n");
}

static void fader_encoder_steps() {
  open_page();
  turn(0, 1);
  CHECK(scene::state().fader == 128);
  turn(0, 1, true);
  CHECK(scene::state().fader == 144);
  turn(0, -5);
  CHECK(scene::state().fader == 0);
  for (int i = 0; i < 140; ++i) turn(0, 1);
  CHECK(scene::state().fader == scene::kFaderMax);
  CHECK(shows("FADER:16383") && shows("100%"));
  scene::set_ab(0, 1);   // a scene switch: the fader stays where it is (relative, no jump)
  turn(0, -1);
  CHECK(scene::state().fader == scene::kFaderMax - 128);
  printf("  fader_encoder_steps: A 128 a detent, FUNC 16, clamped 0..16383, relative\n");
}

static void hold_shows_lock_list_and_edits() {
  open_page();
  for (int k = 0; k < 9; ++k) {
    char n[8];
    snprintf(n, sizeof n, "V1:P%d", k + 1);
    CHECK(scene::lock(2, T(n), (uint16_t)((10 + k) * 128)));
  }
  down(0x02);
  CHECK(!shows("S3 LOCKS"));   // a tap's time: the main view
  tick(10);                    // held 300 ms: the list
  // 2026-11: a row names the param as voice 1's machine (PLAITS) does: P1 ENG, P7 TUN, P8 LEV, P9 FM (its page 2)
  CHECK(shows("S3 LOCKS") && shows("V1:ENG") && shows("V1:TUN") && !shows("V1:LEV") && shows("1-7/9") && !shows("V1:P1"));
  turn(1, 1);                  // B: row 1 + 1
  CHECK(lock_v(2, T("V1:P1")) == 11 * 128 && shows("V1:ENG:11"));
  turn(3, 2, true);            // D: row 3 + 16 (FUNC x 8)
  CHECK(lock_v(2, T("V1:P3")) == 28 * 128);
  tap(0x41), tap(0x41);        // RIGHT x 2: rows 3..9 (a third: the ADD row on top, v2)
  CHECK(shows("3-9/9") && shows("V1:FM") && !shows("V1:HRM"));
  turn(7, -1);                 // H: row 9
  CHECK(lock_v(2, T("V1:P9")) == 17 * 128);
  up(0x02);
  CHECK(!shows("S3 LOCKS") && shows("MORPH"));
  // a value never past its range: T1:DIR 0..5
  CHECK(scene::lock(5, T("T1:DIR"), 3 * 128));
  down(0x05), turn(1, 5);
  CHECK(lock_v(5, T("T1:DIR")) == 5 * 128);
  up(0x05);
  printf("  hold_shows_lock_list_and_edits: held 250 ms the list, B..H edit (FUNC x 8), LEFT / RIGHT scroll\n");
}

static void no_removes_row() {
  open_page();
  CHECK(scene::lock(0, T("V1:P1"), 128) && scene::lock(0, T("MFX:LEV"), 64 * 128) && scene::lock(0, T("V2:P2"), 256));
  down(0x00);
  tap(0x45);                   // NO: B's row (V1:P1) goes
  CHECK(scene::state().s[0].n == 2 && lock_v(0, T("V1:P1")) < 0 && lock_v(0, T("MFX:LEV")) == 64 * 128);
  tap(0x45), tap(0x45), tap(0x45);   // NO on an empty list: nothing, the page stays
  CHECK(scene::state().s[0].n == 0 && !strcmp(ui::remote_page_title(), "MORPH") && shows("S1 ADD"));
  up(0x00);
  int closes = g_closes;
  tap(0x45);                   // NO with no scene key held: back (nav044: a shortcut's page asks itself closed)
  CHECK(g_closes == closes + 1 || !ui::remote_page_title() || strcmp(ui::remote_page_title(), "MORPH"));
  printf("  no_removes_row: NO while held removes B's row; without a hold NO is back\n");
}

static void yes_clears_after_confirm() {
  open_page();
  CHECK(scene::lock(3, T("V1:P1"), 128) && scene::lock(3, T("V1:P2"), 128));
  down(0x03), tap(0x44);       // YES while held: asks
  CHECK(scene::state().s[3].n == 2 && shows("CLEAR S4? YES AGAIN"));
  tap(0x45);                   // NO: kept
  CHECK(scene::state().s[3].n == 2 && !shows("CLEAR S4?"));
  tap(0x44), up(0x03);         // asked again; the key let go: the question stays
  CHECK(shows("CLEAR S4? YES AGAIN"));
  tap(0x44);                   // YES again: cleared
  CHECK(scene::state().s[3].n == 0 && !shows("CLEAR S4?"));
  printf("  yes_clears_after_confirm: hold + YES asks, NO keeps, YES again clears\n");
}

// 0.5.0 v2 (S7): no SCN / PERF cells: G and H do nothing on the main view
static void no_scn_perf_cells() {
  open_page();
  scene::set_ab(0, 1);
  tap(0x01);
  turn(6, 1), turn(7, 1), turn(6, -3), turn(7, -2);
  CHECK(scene::state().s[1].n == 0 && scene::state().s[0].n == 0);
  CHECK(!shows("SCN") && !shows("PERF") && shows("FADER") && shows("LOCKS"));
  printf("  no_scn_perf_cells: G / H do nothing, no SCN / PERF cells\n");
}

// ---- v2 Task 5: the ADD row, the last of a held scene's list (on top: B track, C page, D param, E value, YES adds)
static void hold_add(int key) {   // the scene key held, scrolled until ADD is the top row
  down(key), tick(10);
  for (int i = 0; i < 70; ++i) tap(0x41);
  CHECK(shows(" ADD"));
}
static void add_row_rt() {
  open_page();
  hold_add(0x02);
  CHECK(shows("S3 ADD") && shows("RT1") && shows("SRC"));
  turn(2, 2);                  // C: SRC SMPL FLTR
  turn(3, 5);                  // D: FENV FATK FDEC FSUS FREL FRQ
  turn(4, 23);                 // E: 64 (the Rytm's own: no base here) + 23
  CHECK(shows("FLTR") && shows("FRQ") && shows("87"));
  tap(0x44);                   // YES adds
  CHECK(lock_v(2, T("RT1:FRQ")) == 87 * 128 && scene::state().s[2].n == 1);
  up(0x02);
  printf("  add_row_rt: hold T3, ADD: RT1 FLTR FRQ 87, YES -> scene 3 has RT1:FRQ 87\n");
}
static void add_row_rfx() {
  open_page();
  hold_add(0x04);
  turn(1, 12);                 // B: RT1..RT12, RFX
  turn(2, 1);                  // C: DELAY REVERB
  turn(3, 1);                  // D: PRE RDEC
  turn(4, 36);
  CHECK(shows("RFX") && shows("REVERB") && shows("RDEC"));
  tap(0x44);
  CHECK(lock_v(4, T("RFX:RDEC")) == 100 * 128);
  up(0x04);
  printf("  add_row_rfx: ADD RFX REVERB RDEC 100\n");
}
static void add_row_voice() {
  open_page();
  uint8_t m[39] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(kPlaits | 0)};   // V1 PLAITS, its knobs at 40
  for (int k = 0; k < 16; ++k) m[6 + 2 * k] = 40, m[7 + 2 * k] = 0;
  m[38] = 0xF7;
  engine_sysex(m, 39);
  tick();
  hold_add(0x00);
  turn(1, 13);                 // B: .. RFX, V1
  CHECK(shows("V1") && shows("ENG"));
  turn(4, 5);                  // E: from the base (40) + 5
  tap(0x44);
  CHECK(lock_v(0, T("V1:P1")) == 45 * 128);
  up(0x00);
  printf("  add_row_voice: ADD V1 ENG (its machine's name) from the base 40 + 5\n");
}
static void add_row_full_refused() {
  open_page();
  for (int k = 0; k < 64; ++k) CHECK(scene::lock(3, mod::tid(mod::kV, k / 16, k % 16), 128));
  hold_add(0x03);
  tap(0x44);
  CHECK(scene::state().s[3].n == 64 && lock_v(3, T("RT1:P1")) < 0 && shows("SCENE FULL"));
  up(0x03);
  printf("  add_row_full_refused: a 65th lock by ADD: SCENE FULL\n");
}

static void leds() {
  open_page();
  CHECK(scene::lock(8, T("V1:P1"), 128));
  scene::set_ab(0, 1);
  tap(0x05);
  tick();
  CHECK(g_leds_on && g_led[0] == ui::kLedA && g_led[1] == ui::kLedB && g_led[5] == 111 && g_led[8] == 107 &&
        g_led[2] == ui::kLedOff && g_led[12] == ui::kLedOff && g_led[13] == ui::kLedOff && g_led[14] == ui::kLedA &&
        g_led[15] == ui::kLedB);
  printf("  leds: A blue, B orange, selected white, locks dim white, T13 / T14 off, T15 / T16 A / B\n");
}

static void text_fit() {
  open_page();   // the longest texts: S12, 64 locks with the longest names, every footer
  for (int k = 0; k < 64; ++k) {
    static const char* const kN[] = {"RT12:FTYP", "I8:A4", "MFX:MIX4", "X8:B3", "O8:WID", "V8:P16", "FX2:P5", "S12:F2"};
    Tid t = T(kN[k % 8]);
    if (k >= 8) t = mod::tid(mod::kV, k % 8, k / 8);
    scene::lock(11, t, (uint16_t)(k % 2 ? 16383 : 127 * 128 + 64));
  }
  CHECK(scene::state().s[11].n == 64);
  CHECK(!scene::lock(11, T("FX1:P2"), 0) && scene::state().s[11].n == 64);   // the 65th: SCENE FULL
  scene::set_ab(10, 11), scene::set_fader(16383, scene::kConsole);
  tap(0x0B), turn(0, 1), draw();
  hold_add(0x0B), tap(0x44);   // ADD on a full scene
  CHECK(shows("SCENE FULL"));
  up(0x0B);
  png("morph_full.png");
  down(0x0B), tick(10);
  png("morph_list.png");
  for (int i = 0; i < 64; ++i) tap(0x41), draw();
  turn(1, 1), draw(), tap(0x44), draw();
  png("morph_clear.png");
  up(0x0B), draw();
  tap(0x45), draw();
  printf("  text_fit: every state drawn with S12, 64 locks, the longest names (TEXT-FIT checks at exit)\n");
}

// ---- Task 39: EDIT on the other Sidecar pages
static bool sent_has(const char* s) {   // a text item the Rytm has now (the host's last commit)
  uint8_t t[36 * ui::kTextSlots];
  int n = ui::sent_texts(t, ui::kTextSlots);
  for (int i = 0; i < n; ++i)
    if (strstr((const char*)t + 36 * i + 4, s)) return true;
  return false;
}
static void voice_page() {   // V1 a PLAITS voice (its report), the page open on it: SIDECAR, RIGHT x 2 (SYSTEM, V1)
  ui::remote_reset();
  ui::morph_page_reset();
  scene::reset();
  uint8_t m[39] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(kPlaits | 0)};
  for (int k = 0; k < 16; ++k) m[6 + 2 * k] = 40, m[7 + 2 * k] = 0;
  m[38] = 0xF7;
  engine_sysex(m, 39);
  from_rytm(0x30, 0, 1, 0);
  tick();
  tap(0x41), tap(0x41);
  tick();
  CHECK(ui::remote_stats().voice == 0);
}
static int enc_param(int e) {   // the voice page's encoder e: its P (0..15)
  const ui::Page* p = ui::page_for_voice(kPlaits, 0);
  CHECK(p && p->enc[e] != ui::kNone);
  return p->enc[e] & 15;
}

static void page(bool down_) { from_rytm(0x31, 0x84 >> 7, 0x84 & 0x7F, down_ ? 1 : 0), tick(1); }
static void side(int key) { page(true), tick(), down(key), tick(); }   // PAGE + T15 / T16 held (PAGE still down)

static void page_t15_turn_locks_a() {
  voice_page();
  scene::set_ab(2, 6);
  tick();
  CHECK(!(g_mask_trigs & 0xFFFF));          // nothing held: the trig keys are the Rytm's
  page(true), tick();
  CHECK((g_mask_trigs & 0xFFFF) == 0xC000); // PAGE held: T15 / T16 the host's
  down(0x0E), tick();
  CHECK((g_mask_trigs & 0xFFFF) == 0xCFFF && scene::edit_side() == 0);   // EDIT A: T1..T12 too
  int k = enc_param(1);
  uint32_t edits = ui::remote_stats().edits;
  float before = v[0].p[k];
  turn(1, 3);                               // scene A (3) locks V1's P, from the base (40) + 3
  CHECK(lock_v(2, mod::tid(mod::kV, 0, k)) == 43 * 128);
  page(false), page(true), page(false);     // PAGE let go first (and again): EDIT stays while T15 is held
  turn(1, 1);
  CHECK(lock_v(2, mod::tid(mod::kV, 0, k)) == 44 * 128);
  CHECK(ui::remote_stats().edits == edits && v[0].p[k] == before && ui::remote_stats().voice == 0);   // no page step
  up(0x0E), tick();
  CHECK(scene::edit_side() < 0 && !(g_mask_trigs & 0xFFFF));
  turn(1, 1);                               // nothing held: the page's own edit, as always
  CHECK(ui::remote_stats().edits == edits + 1 && lock_v(2, mod::tid(mod::kV, 0, k)) == 44 * 128);
  // MASTER: its LEV (H) under PAGE + T15: scene A (3) locks MFX:LEV, the master's knob stays
  core::ins::set_type(core::master::chain(0), core::master::slot(0), core::ins::kComp);   // a master slot: MASTER shown
  ui::mfx_page_set(0, true);   // its EQ view: H the master's LEV
  ui::remote_show_page("MASTER");
  tick();
  CHECK(ui::remote_page_title() && !strcmp(ui::remote_page_title(), "MASTER"));
  int lev = core::master::control.lev;
  side(0x0E), turn(7, 2);
  CHECK(lock_v(2, mod::tid(mod::kMfx, 0, 0)) == (lev + 2) * 128 && core::master::control.lev == lev);
  up(0x0E), page(false);
  CHECK(!strcmp(ui::remote_page_title(), "MASTER"));
  core::ins::set_type(core::master::chain(0), core::master::slot(0), 0);
  printf("  page_t15_turn_locks_a: PAGE + T15 held + a voice knob locks scene A (no set_param, no page step); MASTER's LEV\n");
}

static void page_t16_turn_locks_b() {
  voice_page();
  scene::set_ab(2, 6);
  side(0x0F);
  int k = enc_param(1);
  turn(1, 2);
  CHECK(lock_v(6, mod::tid(mod::kV, 0, k)) == 42 * 128 && scene::state().s[2].n == 0 && scene::edit_side() == 1);
  up(0x0F), page(false);
  printf("  page_t16_turn_locks_b: PAGE + T16 locks scene B\n");
}

static void t15_without_a_refused() {
  voice_page();
  side(0x0E);
  uint32_t edits = ui::remote_stats().edits;
  turn(1, 2);
  tick();
  CHECK(sent_has("SET A: T1-12") && ui::remote_stats().edits == edits);
  for (int n = 0; n < scene::kScenes; ++n) CHECK(scene::state().s[n].n == 0);
  up(0x0E), page(false);
  printf("  t15_without_a_refused: no A: the turn is refused, SET A: T1-12\n");
}

static void page_t15_plus_t3_sets_a_on_voice_page() {
  voice_page();
  scene::set_ab(0, 6), scene::set_fader(5000, scene::kConsole);
  side(0x0E);
  tap(0x02);                                // T3: scene 3 becomes A; the fader stays
  CHECK(scene::state().a == 2 && scene::state().b == 6 && scene::state().fader == 5000);
  turn(1, 1);
  CHECK(scene::state().s[2].n == 1);
  up(0x0E), page(false);
  side(0x0F), tap(0x04), up(0x0F), page(false);   // PAGE + T16 + T5: B = 5
  CHECK(scene::state().a == 2 && scene::state().b == 4 && ui::remote_stats().voice == 0);
  printf("  page_t15_plus_t3_sets_a_on_voice_page: PAGE + T15 + T3: A = 3; PAGE + T16 + T5: B = 5\n");
}

static void t1_alone_not_a_scene_key() {
  voice_page();
  scene::set_ab(0, 1);
  int k = enc_param(1);
  uint32_t edits = ui::remote_stats().edits;
  down(0x00), turn(1, 1), up(0x00);         // (a T1 the Rytm keeps: no EDIT)
  down(0x0E), turn(1, 1), up(0x0E);         // T15 without PAGE: the Rytm's too
  CHECK(scene::state().s[0].n == 0 && scene::state().a == 0 && ui::remote_stats().edits == edits + 2);
  (void)k;
  printf("  t1_alone_not_a_scene_key: T1 / T15 alone are the Rytm's, a turn edits the voice\n");
}

static void page_alone_still_steps() {
  voice_page();
  page(true), tick();
  CHECK(ui::remote_stats().voice == 0);     // held back while it may start the gesture
  page(false), tick();
  CHECK(ui::remote_stats().voice != 0);     // let go alone: PAGE's own step, as always
  printf("  page_alone_still_steps: PAGE alone steps the page at its release\n");
}

static void edit_released_on_kit_change() {
  voice_page();
  scene::set_ab(2, 6);
  side(0x0E);
  CHECK(scene::edit_side() == 0);
  const uint8_t kit[8] = {0xF0, 0x7D, 0x52, 0x54, 0x3B, 1, 4, 0xF7};
  engine_sysex(kit, 8);
  tick();
  CHECK(scene::edit_side() < 0);
  uint32_t edits = ui::remote_stats().edits;
  turn(1, 1);                               // the gesture let go: an edit, no lock
  CHECK(ui::remote_stats().edits == edits + 1);
  up(0x0E), page(false);
  printf("  edit_released_on_kit_change: a kit change ends the held EDIT\n");
}

static void edit_off_trigs_stock() {
  voice_page();
  CHECK(!(g_mask_trigs & 0xFFFF));          // nothing held: the trig keys are the Rytm's on a voice page
  int k = enc_param(1);
  uint32_t edits = ui::remote_stats().edits;
  down(0x02), turn(1, 1), up(0x02);         // (a T3 the Rytm would keep: nothing locks)
  CHECK(scene::state().s[2].n == 0 && ui::remote_stats().edits == edits + 1);
  (void)k;
  printf("  edit_off_trigs_stock: no PAGE + T15 / T16 - no trig keys caught, a turn edits the voice\n");
}

static void locked_value_shown_while_held() {
  voice_page();
  scene::set_ab(2, 6);
  tick();
  CHECK(!sent_has("EDIT"));
  side(0x0E);
  tick();
  CHECK(sent_has("BD EDIT A"));             // the header keeps the voice (its track: UX 7's "V1 EDIT A")
  turn(1, 5);
  tick();
  char want[32], nm[16];
  mod::label(mod::tid(mod::kV, 0, enc_param(1)), nm, sizeof nm);   // 2026-11: the param by its machine's name ("V1:HRM")
  CHECK(strncmp(nm, "V1:P", 4) != 0);
  snprintf(want, sizeof want, "S3 %s:45", nm);
  CHECK(sent_has(want));                    // the bottom bar: the scene's locked value
  up(0x0E), page(false);
  tick();
  CHECK(!sent_has(want) && !sent_has("EDIT"));
  printf("  locked_value_shown_while_held: BD EDIT A (the voice kept) in the header, the locked value in the bottom bar\n");
}

// ---- review: seq mode (a Sidecar seq track selected: 2D 01 08 1 t) sends every key and turn to the sequencer first
static void seq_mode(bool on) { from_rytm(0x2D, 8, on ? 1 : 0, 0), tick(); }

static void seq_mode_morph_knobs() {
  open_page();
  seq_mode(true);
  ui::remote_show_page("MORPH");
  tick();
  CHECK(!strcmp(ui::remote_page_title(), "MORPH"));
  turn(0, 2);                               // A: the fader, not the sequencer's
  CHECK(scene::state().fader == 256);
  tap(0x01);
  CHECK(scene::lock(1, T("V1:P1"), 10 * 128));
  down(0x01), turn(1, 1);                   // the list: B edits row 1 (V1:P1)
  CHECK(lock_v(1, T("V1:P1")) == 11 * 128);
  up(0x01);
  CHECK(ui::morph_page_held() == -1);
  seq_mode(false);
  printf("  seq_mode_morph_knobs: in seq mode A and the list's knobs are MORPH's\n");
}

static void seq_mode_edit_lock() {
  voice_page();
  seq_mode(true);                           // (it shows SEQ: back to V1 from SIDECAR)
  ui::remote_show_page("SIDECAR");
  tick(), tap(0x41), tap(0x41), tick();
  CHECK(ui::remote_stats().voice == 0);
  scene::set_ab(2, 6);
  int k = enc_param(1);
  side(0x0E), turn(1, 2);
  CHECK(lock_v(2, mod::tid(mod::kV, 0, k)) == 42 * 128);
  up(0x0E), page(false);
  seq_mode(false);
  printf("  seq_mode_edit_lock: in seq mode PAGE + T15 + a voice knob still locks\n");
}

static void release_while_seq_shown() {
  open_page();
  down(0x03);
  tick(10);
  CHECK(ui::morph_page_held() == 3);
  ui::remote_show_page("SEQ");
  tick();
  up(0x03);                                 // released with SEQ shown: still MORPH's release
  CHECK(ui::morph_page_held() == -1);
  voice_page();
  scene::set_ab(2, 6);
  side(0x0E);
  ui::remote_show_page("SEQ");
  tick();
  up(0x0E), page(false);                    // EDIT's key let go on SEQ
  tick();
  CHECK(scene::edit_side() < 0 && !sent_has("EDIT A"));
  ui::remote_show_page("SIDECAR");
  tick(), tap(0x41), tap(0x41), tick();
  uint32_t edits = ui::remote_stats().edits;
  turn(1, 1);                               // no scene key held any more: an edit, no lock
  CHECK(ui::remote_stats().edits == edits + 1 && scene::state().s[2].n == 0);
  printf("  release_while_seq_shown: a held key's release reaches its page even with SEQ shown\n");
}

static void not_lockable_note() {
  voice_page();
  scene::set_ab(0, 1);
  core::ins::set_type(core::master::chain(0), core::master::slot(0), core::ins::kComp);
  ui::mfx_page_set(0, false);
  ui::remote_show_page("MASTER");
  tick();
  side(0x0E), turn(4, 1);                   // E: the slot (a list, no target)
  tick();
  CHECK(sent_has("NOT LOCKABLE") && scene::state().s[0].n == 0);
  up(0x0E), page(false);
  core::ins::set_type(core::master::chain(0), core::master::slot(0), 0);
  printf("  not_lockable_note: a knob with no target under a held scene key says NOT LOCKABLE\n");
}

int main() {
  hooks.to_rytm = out;
  at(g_us);
  engine_begin();
  select_and_ab_by_holds();
  fader_encoder_steps();
  hold_shows_lock_list_and_edits();
  no_removes_row();
  yes_clears_after_confirm();
  no_scn_perf_cells();
  add_row_rt();
  add_row_rfx();
  add_row_voice();
  add_row_full_refused();
  leds();
  text_fit();
  page_t15_turn_locks_a();
  page_t16_turn_locks_b();
  t15_without_a_refused();
  page_t15_plus_t3_sets_a_on_voice_page();
  t1_alone_not_a_scene_key();
  page_alone_still_steps();
  edit_released_on_kit_change();
  edit_off_trigs_stock();
  locked_value_shown_while_held();
  seq_mode_morph_knobs();
  seq_mode_edit_lock();
  release_while_seq_shown();
  not_lockable_note();
  scene::reset();
  ui::remote_reset();
  printf("test_morph_page: ok\n");
  return 0;
}
