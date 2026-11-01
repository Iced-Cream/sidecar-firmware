// pi/engine/tests/test_midiout_q.cpp - MidiOutQ (midiout_q.h): the order, the tiers, note-offs kept under any load, a failed transfer's note-offs back in front.
#include "check.h"
#include "../midiout_q.h"

static uint32_t pkt(uint8_t st, uint8_t a, uint8_t b) { return (uint32_t)(st >> 4) | (uint32_t)st << 8 | (uint32_t)a << 16 | (uint32_t)b << 24; }
static uint32_t rt(uint8_t st) { return 0x0Fu | (uint32_t)st << 8; }

int main() {
  uint32_t o[MidiOutQ::kBatch];
  MidiOutQ q;
  CHECK(q.pop(o) == 0);
  // order, batches
  for (int i = 0; i < 20; ++i) q.push(pkt(0x90, (uint8_t)i, 100));
  CHECK(q.size() == 20 && q.pop(o) == MidiOutQ::kBatch && o[0] == pkt(0x90, 0, 100) && o[15] == pkt(0x90, 15, 100));
  CHECK(q.pop(o) == 4 && o[3] == pkt(0x90, 19, 100) && q.size() == 0);
  // a device that takes nothing: clock stops first, then values, then note-ons; note-offs fill the rest
  for (int i = 0; i < 1000; ++i) q.push(rt(0xF8));
  CHECK(q.size() == MidiOutQ::kRealtimeMax);
  for (int i = 0; i < 1000; ++i) q.push(pkt(0xB0, 1, 2));
  CHECK(q.size() == MidiOutQ::kValueMax);
  for (int i = 0; i < 1000; ++i) q.push(pkt(0x90, 60, 100));
  CHECK(q.size() == MidiOutQ::kNoteMax);
  q.push(rt(0xFA));
  CHECK(q.size() == MidiOutQ::kNoteMax);
  for (int i = 0; i < 64; ++i) q.push(pkt(0x80, (uint8_t)i, 0));
  CHECK(q.size() == MidiOutQ::kCap && q.offs_dropped() == 0);   // the oldest clocks and values made room
  int offs = 0;
  while (q.size()) {
    unsigned n = q.pop(o);
    for (unsigned i = 0; i < n; ++i) offs += MidiOutQ::is_off(o[i]);
  }
  CHECK(offs == 64);
  // velocity 0 is a note-off
  CHECK(MidiOutQ::is_off(pkt(0x90, 60, 0)) && !MidiOutQ::is_on(pkt(0x90, 60, 0)) && MidiOutQ::is_on(pkt(0x90, 60, 1)));
  // only note-offs left and more come: counted, nothing else lost
  q.clear();
  for (int i = 0; i < 70; ++i) q.push(pkt(0x80, (uint8_t)i, 0));
  CHECK(q.size() == MidiOutQ::kCap && q.offs_dropped() == 6);
  // a failed transfer: its note-offs in front, in order; its note-ons and values gone
  q.clear();
  q.push(pkt(0x90, 1, 100));
  q.push(pkt(0x80, 2, 0));
  q.push(pkt(0xB0, 3, 4));
  q.push(pkt(0x80, 5, 0));
  CHECK(q.pop(o) == 4);
  q.push(pkt(0x90, 6, 100));
  q.failed();
  CHECK(q.size() == 3 && q.pop(o) == 3 && o[0] == pkt(0x80, 2, 0) && o[1] == pkt(0x80, 5, 0) && o[2] == pkt(0x90, 6, 100));
  // a second failure repeats them
  q.failed();
  CHECK(q.size() == 2);
  // a wrap of the ring
  q.clear();
  for (int r = 0; r < 100; ++r) {
    for (int i = 0; i < 16; ++i) q.push(pkt(0x90, (uint8_t)i, 100));
    CHECK(q.pop(o) == 16 && o[15] == pkt(0x90, 15, 100));
  }
  // the 1 MHz clock is 32 bits: never armed, nothing holds the device at any time, past 2^31 us (35.8 minutes) above all
  {
    MidiOutQ h;
    CHECK(!h.held(0) && !h.held(0x7FFFFFFFu) && !h.held(0x80000000u) && !h.held(0xC0000000u) && !h.held(0xFFFFFFFFu));
    // armed: held kHoldUs, then free, for good: a time left from long ago reads as the future half a wrap later
    h.hold(1000);
    CHECK(h.held(1000) && h.held(1000 + MidiOutQ::kHoldUs - 1) && !h.held(1000 + MidiOutQ::kHoldUs));
    CHECK(!h.held(1000 + MidiOutQ::kHoldUs + 0x80000000u) && !h.held(1000 - 5));
    // across the wrap
    h.hold(0xFFFFFFF0u);
    CHECK(h.held(0xFFFFFFF8u) && h.held(5) && !h.held(0xFFFFFFF0u + MidiOutQ::kHoldUs));
    // a device gone
    h.hold(0);
    h.clear();
    CHECK(!h.held(1));
  }
  printf("midiout_q: PASS\n");
  return 0;
}
