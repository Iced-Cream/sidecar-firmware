// core/tests/test_admit.cpp - CPU admission of a box's voices at attach (core/device/admit.h; design decided 2026-11-21):
// the cost model (voices per render core by split=, the AMP cap, each box's chains on core 2, the clock's scale), fit (the
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
  // a voice 10 %, a box's chains 20 % on core 2, at 1500 MHz; split=4
  admit::set_costs({1500, 1000, 0, 0, 2000});
  CHECK(admit::on() && admit::clock() == 1500);
  CHECK(admit::core_bp(1, l) == 4000 && admit::core_bp(2, l) == 6000 && admit::core_bp(0, l) == 0);
  // a second box of 8: core 1 takes its 0..3 (80 % exactly: allowed), core 2 its chains (80 %) and none of its 4..7
  CHECK(admit::fit(1, 8, l) == 4);
  CHECK(admit::fit(1, 3, l) == 3);   // never more than it asked for
  // INSERT and AMP: a voice 10 % + 2 %, AMP 5 % for each of up to 6 voices on a core
  admit::set_costs({1500, 1000, 200, 500, 2000});
  CHECK(admit::core_bp(1, l) == 4 * 1200 + 4 * 500);   // 68 %
  CHECK(admit::fit(1, 8, l) == 0);   // core 1: one more voice is 68 + 12 + 5 = 85 %; core 2 already 88 % with its chains
  admit::set_costs({1500, 1000, 200, 300, 1000});
  // core 1: 4 x 12 + 4 x 3 = 60 %, +1 voice 75 %, +2 90 %; core 2: 60 + 2 x 10 = 80 % with no voice of the second box
  CHECK(admit::fit(1, 8, l) == 1);
  admit::Load six = {{8, 8}, {true, true}};   // the AMP cap: 6 of a core's 8 voices
  CHECK(admit::core_bp(1, six) == 8 * 1200 + 6 * 300);
  // the clock: 1000 MHz costs 1.5 x the bench's 1500
  admit::set_costs({1500, 1000, 0, 0, 2000});
  admit::set_clock(1000);
  CHECK(admit::clock() == 1000 && admit::core_bp(1, l) == 6000 && admit::core_bp(2, l) == 9000);
  CHECK(admit::fit(1, 8, l) == 1);   // core 1: 60 + 15 = 75 %; core 2 is over by the chains alone: none of its voices there
  admit::set_clock(1500);
  // split=2: a box's 0..1 on core 1, 2..7 on core 2
  admit::set_split(2);
  admit::Load a4 = {{4, 0}, {true, false}};   // an A4 (4 voices): 2 + 2
  CHECK(admit::core_bp(1, a4) == 2000 && admit::core_bp(2, a4) == 4000);
  CHECK(admit::fit(1, 8, a4) == 4);   // core 1: 2 + 2 = 40 %; core 2: 2 + 2 + 2 x 20 % = 80 %, a 5th is 90 %
  admit::set_split(0);
  CHECK(admit::split() == 1);
  admit::set_split(9);
  CHECK(admit::split() == 7);
  admit::set_split(4);
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

  // the bench: a voice 12 %, a box's chains 10 %; split=4. The A4 attaches first (USB-C): its 4 on core 1 (48 %)
  admit::set_costs({1500, 1200, 0, 0, 1000});
  device::readmit();
  CHECK(hello(0, device::kIdA4) == device::kOk);
  CHECK(device::slot_held_by(&device::kA4) == 0 && device::admitted(0) == 4 && !device::g_links[0].tell);
  // the Rytm second, on the relay: core 1 has room for two more voices (48 + 24 = 72 %, a third 84 %)
  rytm35(1);
  CHECK(device::rytm_slot() == 1 && device::admitted(1) == 2 && device::g_links[1].want == 8 && device::g_links[1].tell);
  CHECK(device::voice_admitted(8 + 1) && !device::voice_admitted(8 + 2) && !device::voice_admitted(8 + 7));
  CHECK(device::voice_admitted(3) && !device::voice_admitted(4));   // the A4's own 0..3 (its 4..7: not its)
  // her reports past her two are dropped; her voice 1 plays
  send(1, trig(1, 60));
  send(1, trig(5, 60));
  CHECK(rms(8 + 1) > 100 && rms(8 + 5) == 0);
  // the A4 leaves: the Rytm is alone and gets her 8 back (and is told)
  device::release(0);
  CHECK(device::admitted(0) == 0 && device::admitted(1) == 8 && device::g_links[1].tell);
  CHECK(device::voice_admitted(0));   // (a free slot: nothing of a box's plays there)
  send(1, trig(5, 62));
  CHECK(rms(8 + 5) > 100);
  for (int i = 0; i < 10; ++i) device::step(true);   // (the A4's saves settle: slot 0 free for any box)

  // a Digitakt now attaches second, beside the Rytm's 8 (core 1 at 48 %): two of its voices fit
  CHECK(hello(0, device::kIdDigitakt) == device::kOk);
  CHECK(device::slot_held_by(&device::kDigitakt) == 0 && device::admitted(0) == 2 && device::admitted(1) == 8);
  // voices= lowers a count (never raises one past its profile's): readmit keeps the first box (the Rytm) first
  CHECK(admit::parse_override("rytm:2,dt:8,a4:6"));
  CHECK(device::voices_asked(nullptr) == 2 && device::voices_asked(&device::kA4) == 4);
  device::readmit();
  // her 2 on core 1; the Digitakt all 8: core 1 24 + 48 = 72 %, core 2 48 + 2 x 10 = 68 %
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
  engine();
  printf("test_admit: PASS\n");
  return 0;
}
