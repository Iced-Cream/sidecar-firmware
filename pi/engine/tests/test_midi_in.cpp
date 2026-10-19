// pi/engine/tests/test_midi_in.cpp - MIDI bytes in USB-sized pieces -> whole SysEx and channel messages: running
// status, realtime bytes inside a SysEx, a broken SysEx dropped, an overlong one dropped.
#include <cstring>
#include <vector>

#include "check.h"
#include "midi_in.h"

static std::vector<std::vector<uint8_t>> sx, ch;
static void on_sx(const uint8_t* d, int n) { sx.emplace_back(d, d + n); }
static void on_ch(uint8_t s, uint8_t a, uint8_t b) { ch.push_back({s, a, b}); }
static std::vector<std::pair<uint8_t, uint16_t>> rt;
static void on_rt(uint8_t s, uint16_t spp) { rt.push_back({s, spp}); }

int main() {
  MidiIn m(on_sx, on_ch);
  const uint8_t in[] = {0x90, 36, 100, 37, 0,          // note on, then running status
                        0xF0, 0x7D, 0xF8, 0x52, 0x54, 0x10, 'A', 0xF7,   // clock inside a SysEx
                        0xB0, 16, 64, 0xC0, 5, 0xF0, 1, 2, 0x80, 36, 0};  // CC, program change, a SysEx cut by a status
  for (uint8_t b : in) m.Feed(b);
  CHECK(sx.size() == 1 && sx[0] == (std::vector<uint8_t>{0xF0, 0x7D, 0x52, 0x54, 0x10, 'A', 0xF7}));
  CHECK(ch.size() == 5);
  CHECK(ch[0] == (std::vector<uint8_t>{0x90, 36, 100}) && ch[1] == (std::vector<uint8_t>{0x90, 37, 0}));
  CHECK(ch[2] == (std::vector<uint8_t>{0xB0, 16, 64}) && ch[3] == (std::vector<uint8_t>{0xC0, 5, 0}));
  CHECK(ch[4] == (std::vector<uint8_t>{0x80, 36, 0}));
  CHECK(m.Clocks() == 1 && m.SysExDropped() == 1);
  m.Feed(0xF0);
  for (int i = 0; i < 600; ++i) m.Feed(1);
  m.Feed(0xF7);
  CHECK(sx.size() == 1 && m.SysExDropped() == 2);   // longer than 512: dropped
  // the realtime callback (STUTTER's clock): F8 FA FB FC, F2 SPP's 14 bits; running status ends at F2
  MidiIn r(on_sx, on_ch, on_rt);
  ch.clear();
  const uint8_t rin[] = {0xFA, 0xF8, 0x90, 40, 1, 0xF2, 0x05, 0x02, 41, 1, 0xF0, 0x7D, 0xF8, 0xF7, 0xFC, 0xFB, 0xFE};
  for (uint8_t b : rin) r.Feed(b);
  CHECK(rt.size() == 6 && rt[0].first == 0xFA && rt[1].first == 0xF8 && rt[2] == (std::pair<uint8_t, uint16_t>{0xF2, 5 | 2 << 7}));
  CHECK(rt[3].first == 0xF8 && rt[4].first == 0xFC && rt[5].first == 0xFB && r.Clocks() == 2);
  CHECK(ch.size() == 1 && ch[0] == (std::vector<uint8_t>{0x90, 40, 1}));   // 41 1 after F2: no running status
  printf("test_midi_in: PASS\n");
  return 0;
}
