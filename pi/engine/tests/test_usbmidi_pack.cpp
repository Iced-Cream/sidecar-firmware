// pi/engine/tests/test_usbmidi_pack.cpp - the engine's SysEx for the Rytm as USB MIDI event packets
// (usbmidi_pack.h): every length 2..300 decodes back (a USB MIDI 1.0 receiver: CIN 4 three bytes, 5/6/7 one/two/
// three and the end), cable 0, padding 0, the core's 13-byte 1D and 43-byte 1A exactly, and refusals.
#include <vector>

#include "check.h"
#include "usbmidi_pack.h"

static std::vector<uint8_t> unpack(const uint8_t* p, int n, bool* ended) {
  std::vector<uint8_t> out;
  *ended = false;
  for (int i = 0; i < n; i += 4) {
    CHECK((p[i] >> 4) == 0);   // cable 0
    int cin = p[i] & 15, k = cin == 4 ? 3 : cin == 5 ? 1 : cin == 6 ? 2 : cin == 7 ? 3 : -1;
    CHECK(k > 0);
    CHECK(!*ended);            // nothing after the end
    for (int j = 0; j < 3; ++j) {
      if (j < k) out.push_back(p[i + 1 + j]);
      else CHECK(p[i + 1 + j] == 0);
    }
    if (cin != 4) *ended = true;
  }
  return out;
}

int main() {
  for (int n = 2; n <= 300; ++n) {
    std::vector<uint8_t> m(n);
    m[0] = 0xF0;
    for (int i = 1; i < n - 1; ++i) m[i] = uint8_t(i * 7 & 0x7F);
    m[n - 1] = 0xF7;
    std::vector<uint8_t> p(usbmidi_sysex_bytes(n) + 4, 0xAA);
    int b = usbmidi_pack_sysex(m.data(), n, p.data());
    CHECK(b == usbmidi_sysex_bytes(n));
    CHECK(p[b] == 0xAA);       // nothing written past it
    bool ended;
    CHECK(unpack(p.data(), b, &ended) == m);
    CHECK(ended);
  }
  // the 13-byte 1D (core/sysex.cpp's sysex_slots(m, 0, 0)): 5 packets, the last CIN 5 with F7 alone
  const uint8_t d1[13] = {0xF0, 0x7D, 0x52, 0x54, 0x1D, 1, 1, 1, 1, 1, 1, 1, 0xF7};
  uint8_t p[256];
  CHECK(usbmidi_pack_sysex(d1, 13, p) == 20);
  const uint8_t want[20] = {4, 0xF0, 0x7D, 0x52, 4, 0x54, 0x1D, 1, 4, 1, 1, 1, 4, 1, 1, 1, 5, 0xF7, 0, 0};
  for (int i = 0; i < 20; ++i) CHECK(p[i] == want[i]);
  // 43 bytes (a 1A part 0): 15 packets, the last CIN 5
  uint8_t a[43] = {0xF0};
  a[42] = 0xF7;
  CHECK(usbmidi_pack_sysex(a, 43, p) == 60 && p[56] == 5 && p[57] == 0xF7);
  // 3n bytes end on CIN 7
  const uint8_t s6[6] = {0xF0, 1, 2, 3, 4, 0xF7};
  CHECK(usbmidi_pack_sysex(s6, 6, p) == 8 && p[0] == 4 && p[4] == 7 && p[7] == 0xF7);
  // refused: not F0 .. F7, a status byte inside, too short
  const uint8_t bad1[3] = {0x90, 60, 100}, bad2[4] = {0xF0, 1, 2, 3}, bad3[5] = {0xF0, 1, 0xF7, 2, 0xF7};
  CHECK(usbmidi_pack_sysex(bad1, 3, p) == 0);
  // a CC (the LFOs, core/lfo.h): one packet, CIN 0xB, cable 0; a SysEx through usbmidi_pack as before; junk refused
  const uint8_t cc[3] = {0xB1, 74, 99};
  CHECK(usbmidi_pack(cc, 3, p) == 4 && p[0] == 0x0B && p[1] == 0xB1 && p[2] == 74 && p[3] == 99);
  CHECK(usbmidi_pack(s6, 6, p) == 8 && p[0] == 4);
  const uint8_t badcc[3] = {0xB1, 0x80, 1}, rt[3] = {0xF8, 0, 0};
  CHECK(usbmidi_pack(badcc, 3, p) == 0 && usbmidi_pack(rt, 3, p) == 0);
  CHECK(usbmidi_pack_sysex(bad2, 4, p) == 0);
  CHECK(usbmidi_pack_sysex(bad3, 5, p) == 0);
  CHECK(usbmidi_pack_sysex(bad2, 1, p) == 0);
  printf("test_usbmidi_pack: ok\n");
  return 0;
}
