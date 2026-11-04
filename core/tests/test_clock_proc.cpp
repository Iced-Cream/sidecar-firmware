// core/tests/test_clock_proc.cpp - the generic services of the engine strip-down: core/clock (2C / 2D parsed into events,
// 4- and 5-byte ticks, block unwrap, malformed messages counted) and core/proc (slots, the parameter queue, process()).
#include <string.h>
#include <vector>

#include "check.h"
#include "../clock/clock.h"
#include "../proc/proc.h"

using namespace core;

static std::vector<clock::Tick> g_ticks;
static std::vector<uint8_t> g_kinds;
static void tick_fn(const clock::Tick& t) { g_ticks.push_back(t); }
static void state_fn(const clock::State& s) { g_kinds.push_back(s.kind); }

static std::vector<uint8_t> msg(uint8_t cmd, std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static std::vector<uint8_t> blk4(uint32_t b) { return {(uint8_t)(b >> 21 & 0x7F), (uint8_t)(b >> 14 & 0x7F), (uint8_t)(b >> 7 & 0x7F), (uint8_t)(b & 0x7F)}; }
static bool feed(const std::vector<uint8_t>& m) { return clock::sysex(m.data(), (int)m.size()); }

struct Gain : proc::Proc {
  float g = 0;
  int resets = 0;
  void process(const proc::Block& b) override {
    for (int i = 0; i < b.frames; ++i) b.out[0][i] += b.in[0][i] * g;
  }
  void set_param(int p, int v) override { if (p == 0) g = v / 16383.0f; }
  void link_reset() override { ++resets; }
  const char* name() const override { return "gain"; }
};

int main() {
  // ---- clock
  CHECK(clock::on_tick(tick_fn) && clock::on_state(state_fn));
  CHECK(clock::transport() == clock::kStop && clock::pattern() == -1);
  std::vector<uint8_t> b = blk4(1000);
  b.insert(b.end(), {2, 0, 5, 16, 2, 11, 7, 64, 2});   // 2 ticks
  CHECK(feed(msg(0x2C, b)));
  CHECK(g_ticks.size() == 2 && g_ticks[0].track == 0 && g_ticks[0].step == 5 && g_ticks[1].track == 11 && g_ticks[1].len == 64);
  CHECK(g_ticks[0].block == 1000 && clock::transport() == clock::kPlay);
  int st, ln, sp;
  CHECK(clock::step_of(11, &st, &ln, &sp) && st == 7 && ln == 64 && sp == 2);
  CHECK(!clock::step_of(3, &st, &ln, &sp));
  // a 5-byte tick (track 12 + a flag byte)
  b = blk4(1001);
  b.insert(b.end(), {1, 12, 3, 16, 2, 1});
  CHECK(feed(msg(0x2C, b)) && g_ticks.size() == 3 && g_ticks[2].track == 12 && g_ticks[2].extra == 1);
  // malformed tick counted, nothing passed on
  uint32_t bad = clock::stats().bad;
  b = blk4(1002);
  b.insert(b.end(), {2, 0, 1, 16});
  CHECK(feed(msg(0x2C, b)) && clock::stats().bad == bad + 1 && g_ticks.size() == 3);
  // block wrap: 28 bits
  b = blk4((1u << 28) - 2);
  b.insert(b.end(), {1, 0, 0, 16, 2});
  feed(msg(0x2C, b));
  b = blk4(3);
  b.insert(b.end(), {1, 0, 1, 16, 2});
  feed(msg(0x2C, b));
  CHECK(g_ticks.back().block == (int64_t)(1u << 28) + 3);
  // 2D: transport, pattern, switch, tempo, edit raw
  b = {clock::kTransport, clock::kStop};
  for (uint8_t x : blk4(5)) b.push_back(x);
  CHECK(feed(msg(0x2D, b)) && clock::transport() == clock::kStop);
  CHECK(feed(msg(0x2D, {clock::kPattern, 4, 9})) && clock::pattern() == 4 && clock::queued() == 9);
  b = {clock::kSwitch, 4, 9};
  for (uint8_t x : blk4(6)) b.push_back(x);
  CHECK(feed(msg(0x2D, b)) && clock::pattern() == 9 && clock::queued() == -1);
  uint32_t t = 120 * 120;
  CHECK(feed(msg(0x2D, {clock::kTempo, (uint8_t)(t >> 14), (uint8_t)(t >> 7 & 0x7F), (uint8_t)(t & 0x7F)})) && clock::bpm() == 120.0);
  CHECK(feed(msg(0x2D, {clock::kEdit, 1, 0, 0, 0, 0, 0, 0})) && g_kinds.back() == clock::kEdit);
  // not ours
  std::vector<uint8_t> other = msg(0x30, {1, 2});
  CHECK(!clock::sysex(other.data(), (int)other.size()));
  clock::reset();
  CHECK(clock::pattern() == -1 && !clock::step_of(11, &st, &ln, &sp));

  // ---- proc
  float in0[8], out0[8];
  for (int i = 0; i < 8; ++i) in0[i] = 1.0f, out0[i] = 0.0f;
  proc::Block blk = {};
  blk.in[0] = in0, blk.out[0] = out0, blk.frames = 8;
  CHECK(!proc::any() && !proc::set_param(0, 0, 100));   // empty slot: dropped
  proc::process(blk);
  CHECK(out0[0] == 0.0f);                               // nothing registered: OUT untouched
  Gain g;
  CHECK(proc::register_slot(1, &g) && !proc::register_slot(1, &g) && !proc::register_slot(proc::kSlots, &g));
  CHECK(proc::any() && proc::slot(1) == &g);
  CHECK(proc::set_param(1, 0, 16383) && !proc::set_param(1, proc::kParams, 1) && !proc::set_param(1, 0, 16384));
  CHECK(g.g == 0.0f);                                   // queued, not applied until the render side runs
  proc::link_reset();
  proc::process(blk);
  CHECK(g.g == 1.0f && g.resets == 1 && out0[0] == 1.0f && out0[7] == 1.0f);
  proc::clear_slot(1);
  proc::process(blk);
  CHECK(out0[0] == 1.0f && !proc::any());
  printf("test_clock_proc: ok\n");
  return 0;
}
