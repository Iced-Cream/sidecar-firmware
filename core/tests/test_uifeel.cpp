// core/tests/test_uifeel.cpp - 0.5.0 UI feel on the Sidecar's pages (core/ui/page.h detent_step, remote.cpp): a centre
// detent on bipolar knobs, FUNC + turn a coarse step, UP held on a page that scrolls with it is a scroll (no map), and
// UP / DOWN (LEFT / RIGHT) repeat while held. Everything through remote_sysex, as the Rytm sends it.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "core.h"
#include "core_port.h"
#include "engine.h"
#include "lfo.h"
#include "ui/extra.h"
#include "ui/page.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
using namespace core::ui;

static uint64_t g_us = 1000000000;
static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static void out(const uint8_t*, int) {}
static void from_rytm(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c) {
  const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, a, b, c, 0xF7};
  engine_sysex(m, 10);
}
static void tick(int n = 1, int ms = 30) {
  for (int i = 0; i < n; ++i) at(g_us + (uint64_t)ms * 1000), remote_tick(now_ms());
}
static void key(int code, bool down) { from_rytm(0x31, (uint8_t)(code >> 7), (uint8_t)(code & 0x7F), down ? 1 : 0); }
static void tap(int code) { key(code, true), key(code, false), tick(); }
static void turn(int e, int delta, bool func = false) { from_rytm(0x32, (uint8_t)e, (uint8_t)(64 + delta), func ? 1 : 0); }
enum { kLeft = 0x40, kRight = 0x41, kUp = 0x42, kDown = 0x43, kYes = 0x44, kNo = 0x45 };
static bool is(const char* title) { return remote_page_title() && !strcmp(remote_page_title(), title); }

// PLAITS on track 1 (P1..P8 ENG HRM TIM MOR DEC LPG TUN LEV), P7 (TUN, kKnobBi) at `tun`, the others 45; its page open
static void plaits_page(int tun) {
  remote_reset();
  const uint8_t mach[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 1, 44, 0x00, 0xF7};
  engine_sysex(mach, sizeof mach);
  uint8_t r[39] = {0xF0, 0x7D, 0x52, 0x54, 0x03, 0x00};
  for (int k = 0; k < 16; ++k) r[6 + 2 * k] = 45;
  r[6 + 2 * 6] = (uint8_t)tun;
  r[38] = 0xF7;
  engine_sysex(r, sizeof r);
  from_rytm(0x30, 0, 1, 0);
  tick();
  for (int i = 0; i < 8 && remote_stats().page != 1; ++i) tap(kRight);
  CHECK(remote_stats().page == 1 && remote_stats().voice == 0);
}

// ---- 1: the centre detent
static void test_detent() {
  plaits_page(63);
  turn(6, 1);
  CHECK(v[0].p[6] == 64);   // lands on the centre
  for (int i = 0; i < kDetentCounts; ++i) {
    turn(6, 1);
    CHECK(v[0].p[6] == 64);
  }   // and stays through the detent's travel
  turn(6, 1);
  CHECK(v[0].p[6] == 65);   // then leaves
  // a turn that crosses stops on the centre; the overshoot is the detent's
  plaits_page(62);
  turn(6, 5);
  CHECK(v[0].p[6] == 64);
  // from below, the other way: a reversal inside the detent cancels the travel
  turn(6, -1), turn(6, -1);
  CHECK(v[0].p[6] == 64);
  turn(6, 1), turn(6, 1);
  CHECK(v[0].p[6] == 64);
  for (int i = 0; i < kDetentCounts + 1; ++i) turn(6, -1);
  CHECK(v[0].p[6] == 63);
  // a knob with no centre (HRM) keeps its counts
  plaits_page(63);
  turn(1, 1);
  CHECK(v[0].p[1] == 46);
  // a coarse FUNC turn lands on the centre too
  plaits_page(60);
  turn(6, 1, true);
  CHECK(v[0].p[6] == 64);
}

// ---- 2: FUNC + turn, a coarse step: 8 (1/16 of the range) on a 0..127 knob, a zone on a list, an octave on a tune
static void test_func_coarse() {
  plaits_page(40);
  turn(1, 1, true);
  CHECK(v[0].p[1] == 45 + 8);    // HRM
  turn(1, -2, true);
  CHECK(v[0].p[1] == 45 + 8 - 16);
  turn(7, 1, true);
  CHECK(v[0].p[7] == 45 + 8);    // LEV, a bar
  const int z = eng_zone(v[0].p[0], 17);
  at(g_us + 1000000);
  turn(0, 1, true);
  CHECK(eng_zone(v[0].p[0], 17) == z + 1);   // ENG, a list: the next zone, never 8
  at(g_us + 1000000);
  turn(0, -3, true);
  CHECK(eng_zone(v[0].p[0], 17) == z);
  turn(6, 1, true);
  CHECK(v[0].p[6] == 40 + 12);   // TUN: an octave
  at(g_us + 1000);
  turn(6, -1, true);
  CHECK(v[0].p[6] == 40);
  // a fast turn without FUNC (8 counts in a report) still holds at the centre
  plaits_page(64);
  turn(6, 8);
  CHECK(v[0].p[6] == 64 + 8 - kDetentCounts);
  // an octave from the centre is an octave (the detent does not eat its counts), and the other direction too
  plaits_page(64);
  turn(6, 1, true);
  CHECK(v[0].p[6] == 76);
  at(g_us + 1000);
  turn(6, -1, true);
  CHECK(v[0].p[6] == 64);   // (back to the centre: it lands)
  // the REC page's playback level (knob D) as well
  remote_play(PlayInfo{});
  PlayInput in;
  kRecPage.enc(3, 5, true);
  CHECK(remote_play_take(&in) && in.level == 40);
}

// the code pages take the same detent: the LFO page's DEPTH (knob D of its main view, -64..63, 0 none)
static void test_detent_idle() {   // a half-pushed detent is forgotten when the knob is left alone
  plaits_page(63);
  turn(6, 1), turn(6, 1), turn(6, 1);   // 64, acc 2
  at(g_us + 2000000);
  for (int i = 0; i < kDetentCounts; ++i) turn(6, 1);
  CHECK(v[0].p[6] == 64);
}

static void test_detent_lfo() {
  lfo_page_reset();
  lfo::Settings st = lfo::get(0);
  st.depth = -1;
  lfo::set(0, st);
  kLfoPage.enc(3, 1, false);
  CHECK(lfo::get(0).depth == 0);
  for (int i = 0; i < kDetentCounts; ++i) {
    kLfoPage.enc(3, 1, false);
    CHECK(lfo::get(0).depth == 0);
  }
  kLfoPage.enc(3, 1, false);
  CHECK(lfo::get(0).depth == 1);
}

// ---- 3 and 4: the SIDECAR MENU (a list that scrolls with UP / DOWN: its mask claims them)
static void open_menu() {
  remote_reset(), menu_reset();
  from_rytm(0x30, 0, 1, 0);
  tick(30);   // (a hello would be a new link after 600 ms: the page is older than that)
  tap(kYes);
  CHECK(is("MENU"));
}
// the row the cursor is on: the left text drawn inverted
static std::string cursor() {
  Canvas c;
  kMenuPage.draw(c, 0, 1, now_ms() | 1);
  for (const TextItem& t : c.txt)
    if (t.s[0] && (t.flags & kTextInvert) && (t.flags & 3) == kAlignLeft) return t.s;
  return "<none>";
}
// the cursor after k DOWN taps from the top, k = 0..n-1 (the menu again after)
static std::vector<std::string> rows(int n) {
  open_menu();
  std::vector<std::string> r;
  for (int k = 0; k < n; ++k) r.push_back(cursor()), tap(kDown);
  open_menu();
  return r;
}

static void test_up_scrolls() {
  const std::vector<std::string> r = rows(6);
  for (int k = 0; k < 3; ++k) tap(kDown);
  CHECK(cursor() == r[3]);
  key(kUp, true);   // UP held on a page that scrolls with it: its key moved the list ...
  CHECK(cursor() == r[2]);
  from_rytm(0x49, 5, 1, 0), tick();   // ... and the Rytm's "UP held alone": no map over it
  CHECK(is("MENU") && !keys_map_on());
  key(kUp, false), from_rytm(0x49, 5, 0, 0), tick();
  CHECK(is("MENU") && cursor() == r[2]);
  // a page that leaves UP to the Rytm (a voice page, its mask names it for DOWN's page only): the map, as before
  plaits_page(63);
  from_rytm(0x49, 5, 1, 0), tick();
  CHECK(is("KEYS") && keys_map_on());
  from_rytm(0x49, 5, 0, 0), tick();
  CHECK(!is("KEYS") && remote_stats().page == 1);
  // INSERT stands alone and takes no UP / DOWN: the map
  remote_show_page("INSERT"), tick();
  CHECK(is("INSERT"));
  from_rytm(0x49, 5, 1, 0), tick();
  CHECK(is("KEYS"));
  from_rytm(0x49, 5, 0, 0), tick();
  // USER SLOTS is a ring page whose rows UP / DOWN move: no map; SCOPE, a ring
  // page that leaves UP alone: the map
  static const RemoteSlots slots = {[]() { return 0; }, [](int, char*, int, const char**) { return false; },
                                    [](int, char*, int) { return ""; }, [](const char*, char*, int) { return false; },
                                    nullptr, nullptr, nullptr};
  remote_slots(&slots);
  for (const char* title : {"USER SLOTS", "SCOPE"}) {
    remote_show_page(title), tick();
    CHECK(is(title));
    from_rytm(0x49, 5, 1, 0), tick();
    CHECK(is(title) == (strcmp(title, "SCOPE") != 0));
    from_rytm(0x49, 5, 0, 0), tick();
    CHECK(is(title));
  }
  remote_slots(nullptr);
}

static void hold_ms(int ms) {   // the ticks of a held key, every 20 ms
  for (int t = 20; t <= ms; t += 20) tick(1, 20);
}
static void test_repeat() {
  const std::vector<std::string> r = rows(16);
  const int more = (1000 - kRepeatFirstMs) / kRepeatMs + 1;   // the repeats within a second of the press
  key(kDown, true);
  CHECK(cursor() == r[1] && remote_key_repeating());
  hold_ms(1000);
  CHECK(cursor() == r[1 + more]);
  key(kDown, false);   // let go: it stops where it is
  hold_ms(1000);
  CHECK(cursor() == r[1 + more] && !remote_key_repeating());
  // before the first repeat's delay: nothing yet
  open_menu();
  key(kDown, true), hold_ms(kRepeatFirstMs - 20);
  CHECK(cursor() == r[1]);
  key(kDown, false);
  // UP too, back up
  open_menu();
  for (int k = 0; k < 12; ++k) tap(kDown);
  key(kUp, true), hold_ms(1000);
  CHECK(cursor() == r[12 - 1 - more]);
  key(kUp, false);
  // another key pressed: it stops (T6 is nobody's on the MENU)
  open_menu();
  key(kDown, true), hold_ms(300);
  CHECK(remote_key_repeating());
  key(5, true);
  CHECK(!remote_key_repeating());
  key(5, false), key(kDown, false);
  // a modifier let go while a key repeats stops it as well
  open_menu();
  key(5, true), key(kDown, true), hold_ms(300);
  CHECK(remote_key_repeating());
  key(5, false);
  CHECK(!remote_key_repeating());
  key(kDown, false);
  // a page change stops it
  open_menu();
  key(kDown, true), hold_ms(300);
  CHECK(remote_key_repeating());
  remote_show_page("KEYS"), tick(2, 20);
  CHECK(!remote_key_repeating());
  key(kDown, false);
  // a hello (a new link: the releases are lost) stops it
  open_menu();
  key(kDown, true), hold_ms(300);
  from_rytm(0x35, 1, 0, 1);
  CHECK(!remote_key_repeating());
  // a release that never comes: it gives up after kRepeatCapMs
  open_menu();
  key(kDown, true), hold_ms(1000);
  CHECK(remote_key_repeating());
  for (int t = 0; t < kRepeatCapMs; t += 100) tick(1, 100);
  CHECK(!remote_key_repeating());
  key(kDown, false);
  // a voice page: DOWN is the ring's next page, no repeat; UP there is the Rytm's (not claimed for a scroll)
  plaits_page(63);
  key(kDown, true);
  CHECK(!remote_key_repeating());
  key(kDown, false);
}

int main() {
  char tmpl[] = "/tmp/core_uifeel_XXXXXX";
  CHECK(mkdtemp(tmpl));
  CHECK(system((std::string("mkdir -p '") + tmpl + "/Samples' '" + tmpl + "/.rytm'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = out;
  at(g_us);
  CHECK(engine_begin());
  struct { const char* name; void (*fn)(); } tests[] = {{"detent", test_detent}, {"detent_lfo", test_detent_lfo}, {"detent_idle", test_detent_idle}, {"func", test_func_coarse}, {"up_scrolls", test_up_scrolls}, {"repeat", test_repeat}};
  for (auto& t : tests) printf("-- %s\n", t.name), fflush(stdout), t.fn();
  remote_reset();
  system((std::string("rm -rf '") + tmpl + "'").c_str());
  printf("test_uifeel: PASS\n");
  return 0;
}
