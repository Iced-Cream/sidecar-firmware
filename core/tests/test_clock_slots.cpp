// core/tests/test_clock_slots.cpp - core/clock one per slot (S8a): two boxes' 2C / 2D streams interleaved, each slot keeps
// its own transport, pattern, tempo, steps, block unwrap, stats and listeners; a reset on one leaves the other; a slot's
// track count from its box's profile (the Rytm 13, the A4 its own) drops ticks on tracks the box does not have.
#include <vector>

#include "check.h"
#include "../clock/clock.h"
#include "../device/a4.h"
#include "../device/device.h"

using namespace core;

static std::vector<clock::Tick> g_t0, g_t1;
static std::vector<clock::State> g_s1;
static void tick0(const clock::Tick& t) { g_t0.push_back(t); }
static void tick1(const clock::Tick& t) { g_t1.push_back(t); }
static void state1(const clock::State& s) { g_s1.push_back(s); }

static std::vector<uint8_t> msg(uint8_t cmd, std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static std::vector<uint8_t> blk4(uint32_t b) { return {(uint8_t)(b >> 21 & 0x7F), (uint8_t)(b >> 14 & 0x7F), (uint8_t)(b >> 7 & 0x7F), (uint8_t)(b & 0x7F)}; }
static std::vector<uint8_t> ticks(uint32_t blk, std::vector<std::vector<uint8_t>> recs) {
  std::vector<uint8_t> b = blk4(blk);
  b.push_back((uint8_t)recs.size());
  for (auto& r : recs) b.insert(b.end(), r.begin(), r.end());
  return msg(clock::kTickCmd, b);
}
static std::vector<uint8_t> transport(uint8_t st, uint32_t blk) {
  std::vector<uint8_t> b = {clock::kTransport, st};
  auto k = blk4(blk);
  b.insert(b.end(), k.begin(), k.end());
  return msg(clock::kStateCmd, b);
}
static std::vector<uint8_t> tempo(uint32_t bpm) {
  uint32_t t = bpm * 120;
  return msg(clock::kStateCmd, {clock::kTempo, (uint8_t)(t >> 14), (uint8_t)(t >> 7 & 0x7F), (uint8_t)(t & 0x7F)});
}
static bool feed(int s, const std::vector<uint8_t>& m) { return clock::slot(s).sysex(m.data(), (int)m.size()); }

int main() {
  clock::Clock& c0 = clock::slot(0);
  clock::Clock& c1 = clock::slot(1);
  int st, ln, sp;

  // ---- slots and the slot-0 calls
  CHECK(&c0 != &c1 && c0.slot() == 0 && c1.slot() == 1);
  CHECK(&clock::slot(clock::kSlots) == &c0 && &clock::slot(-1) == &c0);   // out of range: slot 0's

  // ---- track counts from the profile: the Rytm 13 (its 12 + the Storage phase's 13th), the A4 its profile's
  const device::Profile* a4 = device::by_id(device::kIdA4);
  CHECK(a4 && a4->tracks > 0 && a4->tracks < clock::kTracks);
  CHECK(clock::tracks_for(nullptr) == clock::kTracks && clock::tracks_for(&device::rytm()) == clock::kTracks);
  CHECK(clock::tracks_for(a4) == a4->tracks);
  CHECK(c0.tracks() == clock::kTracks && c1.tracks() == clock::kTracks);   // unbound: the Rytm's, as the one-box engine
  c1.set_tracks(clock::tracks_for(a4));
  CHECK(c1.tracks() == a4->tracks && c0.tracks() == clock::kTracks && clock::tracks() == clock::kTracks);

  // ---- listeners are the slot's own: the slot-0 calls register on slot 0
  CHECK(clock::on_tick(tick0) && c1.on_tick(tick1) && c1.on_state(state1));

  // ---- two streams interleaved: each slot's transport, tempo, pattern and steps its own
  CHECK(feed(0, transport(clock::kPlay, 1000)));
  CHECK(feed(1, tempo(90)));
  CHECK(feed(0, tempo(120)));
  CHECK(feed(1, msg(clock::kStateCmd, {clock::kPattern, 7, 8})));
  CHECK(feed(0, ticks(1001, {{12, 3, 16, 2}, {0, 4, 16, 2}})));   // the Rytm's 13th track counts
  CHECK(feed(1, ticks(50, {{2, 9, 64, 1}})));
  CHECK(c0.transport() == clock::kPlay && c0.bpm() == 120.0 && c0.pattern() == -1);
  CHECK(c1.transport() == clock::kPlay && c1.bpm() == 90.0 && c1.pattern() == 7 && c1.queued() == 8);   // ticks: playing
  CHECK(c0.step_of(12, &st, &ln, &sp) && st == 3 && c0.step_of(0, &st, &ln, &sp) && st == 4);
  CHECK(!c0.step_of(2, &st, &ln, &sp));
  CHECK(c1.step_of(2, &st, &ln, &sp) && st == 9 && ln == 64 && sp == 1 && !c1.step_of(0, &st, &ln, &sp));
  CHECK(g_t0.size() == 2 && g_t1.size() == 1);
  CHECK(g_t0[0].slot == 0 && g_t0[0].block == 1001 && g_t1[0].slot == 1 && g_t1[0].block == 50);
  CHECK(g_s1.size() == 2 && g_s1[0].slot == 1 && g_s1[0].kind == clock::kTempo);
  CHECK(clock::transport() == clock::kPlay && clock::bpm() == 120.0);   // the slot-0 calls read slot 0

  // ---- a track the slot's box does not have reaches no listener, counted off_track; the other slot takes it
  const int off = a4->tracks;   // the A4's first track past its own
  size_t n1 = g_t1.size(), n0 = g_t0.size();
  CHECK(feed(1, ticks(51, {{(uint8_t)off, 1, 16, 2}})));
  CHECK(g_t1.size() == n1 && c1.stats().off_track == 1 && !c1.step_of(off, &st, &ln, &sp));
  CHECK(feed(0, ticks(1002, {{(uint8_t)off, 5, 16, 2}})));
  CHECK(g_t0.size() == n0 + 1 && c0.stats().off_track == 0 && c0.step_of(off, &st, &ln, &sp) && st == 5);
  CHECK(feed(0, ticks(1003, {{clock::kTracks, 0, 16, 2}})) && c0.stats().off_track == 1);   // past the Rytm's 13

  // ---- stats are the slot's own
  CHECK(c0.stats().ticks == 4 && c1.stats().ticks == 2);
  CHECK(c0.stats().states == 2 && c1.stats().states == 2 && c0.stats().bad == 0 && c1.stats().bad == 0);
  std::vector<uint8_t> bad = msg(clock::kTickCmd, {0, 0, 0, 1, 2, 3});   // 1 tick, 1 byte short
  CHECK(feed(1, bad) && c1.stats().bad == 1 && c0.stats().bad == 0);

  // ---- block unwrap per slot: slot 1's 28-bit wrap leaves slot 0's block numbers
  const uint32_t top = (1u << 28) - 10;
  CHECK(feed(1, ticks(top, {{0, 0, 16, 2}})) && g_t1.back().block == top);
  CHECK(feed(1, ticks(5, {{0, 1, 16, 2}})) && g_t1.back().block == (int64_t)(1u << 28) + 5);
  CHECK(feed(0, ticks(1004, {{1, 0, 16, 2}})) && g_t0.back().block == 1004);

  // ---- a reset on one slot leaves the other
  c1.reset();
  CHECK(c1.transport() == clock::kStop && c1.pattern() == -1 && c1.bpm() == 0 && !c1.step_of(2, &st, &ln, &sp));
  CHECK(c1.tracks() == a4->tracks);   // the box's track count kept: the binding, not the link state
  CHECK(c0.transport() == clock::kPlay && c0.bpm() == 120.0 && c0.step_of(12, &st, &ln, &sp) && st == 3);
  CHECK(feed(1, ticks(7, {{0, 2, 16, 2}})) && g_t1.back().block == 7);   // unwrap starts again on slot 1 only
  clock::reset();   // slot 0's
  CHECK(c0.transport() == clock::kStop && !c0.step_of(12, &st, &ln, &sp));
  CHECK(c1.transport() == clock::kPlay && c1.step_of(0, &st, &ln, &sp) && st == 2);
  CHECK(feed(0, ticks(3, {{1, 1, 16, 2}})) && g_t0.back().block == 3);

  // ---- a smaller track count forgets steps past it; set_tracks clamps
  c0.set_tracks(4);
  CHECK(c0.step_of(1, &st, &ln, &sp) && !c0.step_of(off, &st, &ln, &sp));
  c0.set_tracks(99);
  CHECK(c0.tracks() == clock::kTracks);
  c0.set_tracks(-1);
  CHECK(c0.tracks() == 0);
  c0.set_tracks(clock::kTracks);

  printf("test_clock_slots: ok\n");
  return 0;
}
