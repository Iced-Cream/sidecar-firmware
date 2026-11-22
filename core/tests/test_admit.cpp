// core/tests/test_admit.cpp - CPU admission of a box's voices at attach (core/device/admit.h; design decided 2026-11-21):
// the cost model (voices per render core by each box's split, the AMP cap on the base box's voices, slot 0's chains on core 2
// and slot 1's on core 1 (decided 2026-11-22), the clock's scale), fit (the
// most voices that keep every render core its voices land on under 80 %), voices= per box type, and in the engine: the
// first box keeps its voices, the second gets what fits and its reports past them are dropped, a box left alone gets its
// count back, readmit after the bench. Synthetic costs: the Pi's come from its boot bench (pi/engine/kernel.cpp LogBench).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "device/a4.h"
#include "device/admit.h"
#include "device/device.h"
#include "device/digitakt.h"
#include "engine.h"
#include "host_link.h"
#include "voices.h"

using namespace core;

static void model() {
  admit::reset();
  CHECK(!admit::on());
  admit::Load l = {{8, 0}, {true, false}};
  CHECK(admit::fit(1, 8, l) == 8);   // no bench: nothing refused
  CHECK(admit::load_pct().amp == admit::kAmpLoadPct && admit::load_pct().voice == admit::kVoiceLoadPct &&
        admit::load_pct().chain == admit::kChainLoadPct);
  admit::set_load_pct({100, 100, 100});   // the synthetic costs below as they are (the LT3 factors: lt3() below)
  // a voice 10 %, a box's chains 20 % (slot 0's on core 2, slot 1's on core 1), at 1500 MHz; split=4
  admit::set_costs({1500, 1000, 0, 0, 2000});
  CHECK(admit::on() && admit::clock() == 1500);
  CHECK(admit::core_bp(1, l) == 4000 && admit::core_bp(2, l) == 6000 && admit::core_bp(0, l) == 0);
  // a second box of 8: core 1 has its chains (40 + 20 %) and takes two of its 0..3 (80 % exactly: allowed); core 2 none
  CHECK(admit::fit(1, 8, l) == 2);
  // asking for 3 it splits 2 + 1 (split_for): core 1 80 %, core 2 70 %; never more than it asked for
  CHECK(admit::fit(1, 3, l) == 3);
  CHECK(admit::split_for(8) == 4 && admit::split_for(4) == 2 && admit::split_for(3) == 2 && admit::split_for(1) == 1 &&
        admit::split_for(0) == 4);
  {
    admit::Load l2 = {{8, 0}, {true, true}, {8, 8}};   // slot 1's chains count on core 1 only
    CHECK(admit::core_bp(1, l2) == 4000 + 2000 && admit::core_bp(2, l2) == 4000 + 2000);
  }
  // INSERT and AMP: a voice 10 % + 2 %, AMP 5 % for each of up to 6 voices on a core
  admit::set_costs({1500, 1000, 200, 500, 2000});
  CHECK(admit::core_bp(1, l) == 4 * 1200 + 4 * 500);   // 68 %
  // core 1: 68 % + slot 1's chains, dry (no AMP pair: not the base box) 20 % = 88 %: over by its chains alone; core 2
  // already 98 % with slot 0's chains and their AMP pair (20 + 2 x 5)
  CHECK(admit::fit(1, 8, l) == 0);
  admit::set_costs({1500, 1000, 200, 300, 400});
  // core 1: 4 x 12 + 4 x 3 + 4 = 64 %, +1 voice 76 %, +2 88 %; core 2: 60 + 4 + 2 x 3 = 70 % (no voice there below 5)
  CHECK(admit::fit(1, 8, l) == 1);
  admit::Load six = {{8, 8}, {true, true}};   // AMP: the base box's (slot 0) voices only, 4 a core at split 4
  CHECK(admit::core_bp(1, six) == 8 * 1200 + 4 * 300 + 400);
  admit::set_split(7);                         // the AMP cap: 6 of the base box's 7 on core 1
  CHECK(admit::core_bp(1, six) == 14 * 1200 + 6 * 300 + 400);
  admit::set_split(4);
  {
    admit::Load b1 = {{0, 8}, {false, true}};  // slot 1 alone is the base box: its AMP and its whole chains (on core 1)
    CHECK(admit::core_bp(1, b1) == 4 * 1200 + 4 * 300 + 1000 && admit::core_bp(2, b1) == 4 * 1200 + 4 * 300);
  }
  // LT1's boot bench (2026-11-22, 1500 MHz): a voice 1.59 + 2.05 %, AMP 4.01 %, at the bench's own cost (factors 100). An A4
  // first (4: 2 + 2, its AMP), a Rytm second: all 8 while a box's chains (dry) are at most 42.12 % (core 2: 2 x (3.64 + 4.01)
  // + 4 x 3.64 + chains + the AMP pair 8.02 <= 80 %)
  {
    admit::Load a4 = {{4, 0}, {true, false}, {4, 0}};
    admit::set_costs({1500, 159, 205, 401, 4212});
    CHECK(admit::fit(1, 8, a4) == 8);
    admit::set_costs({1500, 159, 205, 401, 4237});   // core 2 over at its 8th voice
    CHECK(admit::fit(1, 8, a4) == 7);
  }
  // the clock: 1000 MHz costs 1.5 x the bench's 1500
  admit::set_costs({1500, 1000, 0, 0, 2000});
  admit::set_clock(1000);
  CHECK(admit::clock() == 1000 && admit::core_bp(1, l) == 6000 && admit::core_bp(2, l) == 9000);
  CHECK(admit::fit(1, 8, l) == 0);   // core 1 (60 + 30 %) and core 2 (90 %) are over by the chains alone: none of its voices
  admit::set_clock(1500);
  // split=2: a box's 0..1 on core 1, 2..7 on core 2
  admit::set_split(2);
  admit::Load a4 = {{4, 0}, {true, false}};   // an A4 (4 voices): 2 + 2
  CHECK(admit::core_bp(1, a4) == 2000 && admit::core_bp(2, a4) == 4000);
  CHECK(admit::fit(1, 8, a4) == 6);   // core 1: 2 + 20 + 2 = 40 %; core 2: 2 + 20 % + 4 = 80 %, a 7th is 90 %
  admit::set_split(0);
  CHECK(admit::split() == 1);
  admit::set_split(9);
  CHECK(admit::split() == 7);
  admit::set_split(4);
  // admit=report (the two-box load test): fit unchanged, nothing refused; enforce again
  admit::set_costs({1500, 1000, 0, 0, 2000});
  CHECK(admit::enforce() && admit::admit(1, 8, l) == 2);
  admit::set_enforce(false);
  CHECK(admit::admit(1, 8, l) == 8 && admit::admit(1, 3, l) == 3 && admit::admit(1, 12, l) == 8 && admit::fit(1, 8, l) == 2);
  admit::set_enforce(true);
  CHECK(admit::admit(1, 8, l) == 2);
  // voices=
  CHECK(admit::parse_override("a4:2,dt:6"));
  CHECK(admit::override_for("A4") == 2 && admit::override_for("Digitakt") == 6 && admit::override_for("Rytm") == -1);
  CHECK(admit::parse_override("RYTM:5,digitakt:7"));
  CHECK(admit::override_for("rytm") == 5 && admit::override_for("Digitakt") == 7 && admit::override_for("A4") == 2);
  CHECK(!admit::parse_override("md:3") && !admit::parse_override("a4:9") && !admit::parse_override("a4") &&
        !admit::parse_override("a4:x") && !admit::parse_override("a4:3;dt:2"));
  CHECK(admit::override_for("A4") == 2);   // (a refused text changes nothing)
  admit::reset();
  CHECK(admit::override_for("A4") == -1);
}

static std::vector<std::vector<uint8_t>> g_out, g_out1;
// LT3's boot bench (2026-11-22, chains-core1 64fdd7b, 1500 MHz: a voice 1.57 + 2.05 %, AMP 4.02 %, the box row 30.28 %) at
// the LT3 load factors (admit.h): the worst case (AMP on every base-box voice the cap allows, its master pair, both boxes'
// chains incl. send FX) does not fit a Rytm or Digitakt base box even alone (core 2 125 %: chains 33.3 + pair 24.1 + 4 x
// (4.89 + 12.06)), so a box beside it gets none; an A4 base box leaves a second box 2 (core 1: 33.9 + 33.3 + 2 x 4.89 = 77 %).
// Measured on the unit: order 2 (Rytm base, no voice AMP) core 2 ~96 %; order 1 config B (A4 base, 4 voice AMPs, its send FX
// idle: the slot 0 stems gap) 81 / 83 %, which the model reads 87 % on core 1 and ~82 % on core 2 with that send FX left out.
static void lt3() {
  admit::reset();
  admit::set_costs({1500, 157, 205, 402, 3028});
  admit::set_clock(1500);
  admit::Load r = {{8, 0}, {true, false}, {8, 0}}, a = {{4, 0}, {true, false}, {4, 0}};
  CHECK(admit::core_bp(2, r) == 12522 && admit::core_bp(1, r) == 6779);
  CHECK(admit::fit(1, 4, r) == 0 && admit::fit(1, 8, r) == 0);
  CHECK(admit::core_bp(1, a) == 3390 && admit::core_bp(2, a) == 9133);
  CHECK(admit::fit(1, 8, a) == 2);
  admit::Load b = {{4, 8}, {true, true}, {4, 8}};   // order 1 config B as run (all 8): core 1 87 %
  CHECK(admit::core_bp(1, b) == 8675);
  admit::reset();
}

static void out0(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static void out1(const uint8_t* m, int n) { g_out1.emplace_back(m, m + n); }
static void send(int t, const std::vector<uint8_t>& m) { engine_sysex_link(t, m.data(), (int)m.size()); }
static std::vector<uint8_t> sidecar(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xf7);
  return m;
}
static int hello(int t, uint16_t id) {
  std::vector<std::vector<uint8_t>>& o = t ? g_out1 : g_out;
  o.clear();
  send(t, sidecar({device::kCmdHello, 1, (uint8_t)(id >> 7), (uint8_t)(id & 0x7f), 1}));
  return o.empty() ? -1 : o.back()[8];
}
static void rytm35(int t) { send(t, sidecar({0x35, 1, 0, 100, 3})); }
static std::vector<uint8_t> trig(int k, int note) {   // a PLAITS 0x04 on the box's voice k (the Rytm's and the A4's report)
  static const uint8_t pl[16] = {93, 64, 0, 0, 127, 0, 64, 127, 64, 64, 0, 64, 64, 0, 0, 100};
  std::vector<uint8_t> t = {0x04, (uint8_t)(kPlaits | k), (uint8_t)note, 100};
  for (int i = 0; i < 16; ++i) t.push_back(pl[i]), t.push_back(0);
  return sidecar(t);
}
static constexpr int kRows = kBlock * 100;
static int16_t ring[kRows][kVoices];
static double rms(int vi) {
  uint64_t mach[kMachines] = {0};
  memset(ring, 0, sizeof ring);
  for (int b = 0; b < kRows / kBlock; ++b) render_voices(ring, kRows, b * kBlock, 0, kVoices, mach);
  double s = 0;
  for (int i = kRows / 2; i < kRows; ++i) s += (double)ring[i][vi] * ring[i][vi];
  return std::sqrt(s / (kRows / 2));
}

static void engine() {
  if (kVoiceSlots < 2) {
    printf("test_admit: an 8-voice engine: one slot, the engine part skipped\n");
    return;
  }
  char tmpl[] = "/tmp/core-admit-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/.rytm") && fs.mkdir("/.a4") && fs.mkdir("/.digitakt"));
  hooks.to_rytm = out0;
  CHECK(engine_begin());
  device::set_out(1, out1);
  // boot: the presumed Rytm in slot 0 has her 8; no bench yet: a second box would get all of its own
  CHECK(device::admitted(0) == 8 && device::admitted(1) == 0 && device::g_links[0].want == 8);
  CHECK(device::voices_asked(nullptr) == 8 && device::voices_asked(&device::kA4) == 4 && device::voices_asked(&device::kDigitakt) == 8);

  // the bench: a voice 12 %, a box's chains 10 %; split=4. The A4 attaches first (USB-C): its 4 as 2 + 2 (24 % a core)
  admit::set_load_pct({100, 100, 100});   // (synthetic costs as they are)
  admit::set_costs({1500, 1200, 0, 0, 1000});
  device::readmit();
  CHECK(hello(0, device::kIdA4) == device::kOk);
  CHECK(device::slot_held_by(&device::kA4) == 0 && device::admitted(0) == 4 && !device::g_links[0].tell);
  // the Rytm second, on the relay: her chains on core 1 (24 + 10 %), room for three of her voices there (70 %, a fourth 82 %)
  rytm35(1);
  CHECK(device::rytm_slot() == 1 && device::admitted(1) == 3 && device::g_links[1].want == 8 && device::g_links[1].tell);
  CHECK(device::voice_admitted(8 + 2) && !device::voice_admitted(8 + 3) && !device::voice_admitted(8 + 7));
  CHECK(device::voice_admitted(3) && !device::voice_admitted(4));   // the A4's own 0..3 (its 4..7: not its)
  // her reports past her two are dropped; her voice 1 plays
  send(1, trig(1, 60));
  send(1, trig(5, 60));
  CHECK(rms(8 + 1) > 100 && rms(8 + 5) == 0);
  // a USB re-enumeration (a plug on the Pi's other hub port knocks a box off and back, seen on a unit): the A4 leaves and comes
  // back while its slot is kept for it (slothold=, g_hold_ms). The Rytm is not raised meanwhile, and the A4 keeps its place:
  // first, its 4; the Rytm still 3
  uint64_t t = 1000000000ull;
  host_fake_time(true, t);
  device::g_hold_ms = 5000;
  device::release(0);
  CHECK(device::admitted(0) == 0 && device::admitted(1) == 3);
  device::step(true);
  CHECK(device::admitted(1) == 3);   // (not yet: the A4 may come back)
  t += 2000000, host_fake_time(true, t);
  CHECK(hello(0, device::kIdA4) == device::kOk);
  CHECK(device::slot_held_by(&device::kA4) == 0 && device::admitted(0) == 4 && device::admitted(1) == 3);
  CHECK(device::g_links[0].order < device::g_links[1].order);
  // the A4 leaves for good: the Rytm is alone once its slot is no longer kept, and gets her 8 back (and is told)
  device::release(0);
  device::step(true);
  CHECK(device::admitted(1) == 3);
  t += 11000000, host_fake_time(true, t);
  device::step(true);
  CHECK(device::admitted(0) == 0 && device::admitted(1) == 8 && device::g_links[1].tell);
  CHECK(device::voice_admitted(0));   // (a free slot: nothing of a box's plays there)
  host_fake_time(false, 0);
  device::g_hold_ms = 0;
  send(1, trig(5, 62));
  CHECK(rms(8 + 5) > 100);
  for (int i = 0; i < 10; ++i) device::step(true);   // (the A4's saves settle: slot 0 free for any box)

  // a Digitakt now attaches second (slot 0: the base box now), beside the Rytm's 8 and her chains on core 1 (48 + 10 %):
  // one of its voices fits there
  CHECK(hello(0, device::kIdDigitakt) == device::kOk);
  CHECK(device::slot_held_by(&device::kDigitakt) == 0 && device::admitted(0) == 1 && device::admitted(1) == 8);
  // admit=report: the second box gets its 8, /status's core_bp shows the worst case of both; the Pi's own notes as either box's
  // (engine_sysex_self_slot: the load test's drone on stock boxes) play its voices; enforce again: back to 1
  admit::set_enforce(false);
  device::readmit();
  CHECK(device::admitted(0) == 8 && device::admitted(1) == 8);
  {
    admit::Load both = {{8, 8}, {true, true}};
    CHECK(admit::core_bp(1, both) == 8 * 1200 + 1000 && admit::core_bp(2, both) == 8 * 1200 + 1000);
  }
  {
    std::vector<uint8_t> a = trig(6, 60), b = trig(3, 64);
    engine_sysex_self_slot(0, a.data(), (int)a.size());   // the Digitakt's voice 6
    engine_sysex_self_slot(1, b.data(), (int)b.size());   // the Rytm's voice 3
    engine_sysex_self_slot(2, b.data(), (int)b.size());   // (no such slot: nothing)
    CHECK(rms(6) > 100 && rms(8 + 3) > 100);
  }
  admit::set_enforce(true);
  device::readmit();
  CHECK(device::admitted(1) == 8 && device::admitted(0) == 1);
  {
    std::vector<uint8_t> a = trig(5, 62);
    engine_sysex_self_slot(0, a.data(), (int)a.size());   // past the Digitakt's one: dropped
    CHECK(rms(5) == 0);
  }
  // loadtest=on (idle_nosave): the flag only (the Pi's console; idle_save writes nothing while on)
  idle_nosave(true);
  CHECK(idle_nosave());
  idle_nosave(false);
  // voices= lowers a count (never raises one past its profile's): readmit keeps the first box (the Rytm) first
  CHECK(admit::parse_override("rytm:2,dt:8,a4:6"));
  CHECK(device::voices_asked(nullptr) == 2 && device::voices_asked(&device::kA4) == 4);
  device::readmit();
  // her 2 as 1 + 1; the Digitakt all 8: core 1 12 + 10 + 48 = 70 %, core 2 12 + 10 + 48 = 70 %
  CHECK(device::admitted(1) == 2 && device::admitted(0) == 8);
  // no bench: everything its count
  admit::reset();
  device::readmit();
  CHECK(device::admitted(1) == 8 && device::admitted(0) == 8);
  device::reset();
  CHECK(device::admitted(0) == 8 && device::admitted(1) == 0);
}

int main() {
  model();
  lt3();
  engine();
  printf("test_admit: PASS\n");
  return 0;
}
