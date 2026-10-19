// pi/engine/tests/test_emu_proto.cpp - the host side of rytm-emu's link (src/oblink.rs): every message the
// emulator sends, split at every byte boundary, gives the same events; R/T/P skipped; a bad byte is an error.
#include <cstring>
#include <vector>

#include "check.h"
#include "emu_proto.h"

struct Rec : EmuParser::Sink {
  uint32_t want = 0;
  std::vector<uint8_t> midi;
  int hello = -1;
  void Want(uint32_t n) override { want += n; }
  void Midi(const uint8_t* d, int n) override { midi.insert(midi.end(), d, d + n); }
  void Hello(uint8_t g) override { hello = g; }
};

static std::vector<uint8_t> stream() {
  std::vector<uint8_t> s = {'N', 0, 2, 0, 0, 'H', 1, 7, 'T', 1, 60, 100, 'P', 1, 1, 2, 3, 4, 5, 6, 7, 8};
  s.insert(s.end(), {'R', 2, 0, 0, 0});
  for (int i = 0; i < 96; ++i) s.push_back((uint8_t)i);
  s.insert(s.end(), {'M', 3, 0, 0xF0, 0x7D, 0xF7, 'N', 32, 0, 0, 0});
  return s;
}

int main() {
  std::vector<uint8_t> s = stream();
  for (size_t cut = 1; cut <= s.size(); ++cut) {   // in two pieces, cut anywhere
    Rec r;
    EmuParser p(&r);
    CHECK(p.Feed(s.data(), (int)cut) && p.Feed(s.data() + cut, (int)(s.size() - cut)));
    CHECK(r.want == 544 && r.hello == 7 && r.midi == (std::vector<uint8_t>{0xF0, 0x7D, 0xF7}));
    CHECK(p.Skipped() == 4 + 10 + 5 + 96);
  }
  {   // byte by byte
    Rec r;
    EmuParser p(&r);
    for (uint8_t b : s) CHECK(p.Feed(&b, 1));
    CHECK(r.want == 544 && r.midi.size() == 3);
  }
  {
    Rec r;
    EmuParser p(&r);
    const uint8_t bad[] = {'Z'};
    CHECK(!p.Feed(bad, 1));
    CHECK(p.Bad());
    const uint8_t good[] = {'N', 1, 0, 0, 0};   // sticky: even a valid message is refused until Reset
    CHECK(!p.Feed(good, 5) && r.want == 0);
    p.Reset();
    CHECK(p.Feed(good, 5) && r.want == 1);
  }
  {   // R with n = 0x5B000000 frames: 48 * n wraps a u32 (to 0xB0000000... or less); the skip must not
    Rec r;
    EmuParser p(&r);
    const uint8_t hdr[] = {'R', 0x00, 0x00, 0x00, 0x5B};   // n = 1526726656; 48*n = 73282879488 > 2^32
    uint8_t junk[1000] = {0};
    CHECK(p.Feed(hdr, 5) && p.Feed(junk, 1000));
    CHECK(p.Skipped() == 5 + 1000);
    // still inside the body, not desynced: an 'N' byte value in the payload is not parsed as a message
    uint8_t n_in_body[] = {'N', 9, 9, 9, 9};
    CHECK(p.Feed(n_in_body, 5) && r.want == 0 && p.Skipped() == 5 + 1000 + 5);
    // n = 2^32-1 (the maximum) also just skips
    EmuParser q(&r);
    const uint8_t big[] = {'R', 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(q.Feed(big, 5) && q.Feed(junk, 1000) && q.Skipped() == 1005);
  }
  {   // M of length 0 delivers nothing; length > 255 (two length bytes) delivers all of it
    Rec r;
    EmuParser p(&r);
    const uint8_t m0[] = {'M', 0, 0, 'N', 2, 0, 0, 0};
    CHECK(p.Feed(m0, sizeof m0) && r.midi.empty() && r.want == 2);
    std::vector<uint8_t> m;
    m.push_back('M'); m.push_back(0x2C); m.push_back(0x01);   // 300
    for (int i = 0; i < 300; ++i) m.push_back((uint8_t)i);
    m.push_back('N'); m.push_back(1); m.push_back(0); m.push_back(0); m.push_back(0);
    CHECK(p.Feed(m.data(), 100) && p.Feed(m.data() + 100, (int)m.size() - 100));
    CHECK(r.midi.size() == 300 && r.midi[299] == (uint8_t)299 && r.want == 3);
  }
  uint8_t m[8];
  CHECK(emu_hello(m, 7) == 3 && m[0] == 'H' && m[1] == 1 && m[2] == 7);
  CHECK(emu_midi_head(m, 300) == 3 && m[0] == 'M' && m[1] == 44 && m[2] == 1);
  CHECK(emu_frames_head(m, 'a', 70000) == 5 && m[0] == 'a' && m[1] == 0x70 && m[2] == 0x11 && m[3] == 1 && m[4] == 0);
  printf("test_emu_proto: PASS\n");
  return 0;
}
