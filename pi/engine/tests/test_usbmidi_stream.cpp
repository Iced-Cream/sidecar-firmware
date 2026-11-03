// pi/engine/tests/test_usbmidi_stream.cpp - usbmidi_stream.h: bytes to packets and back, SysEx of every length,
// running status, real time inside a SysEx, stray bytes.
#include <stdio.h>

#include <vector>

#include "check.h"
#include "usbmidi_pack.h"
#include "usbmidi_stream.h"

using Bytes = std::vector<uint8_t>;

static Bytes Packets(const Bytes& stream) {
  UsbMidiFromStream s;
  Bytes out;
  for (uint8_t b : stream) s.Feed(b, [&](const uint8_t* p) { out.insert(out.end(), p, p + 4); });
  return out;
}

static Bytes Stream(const Bytes& packets) {
  Bytes out;
  for (size_t i = 0; i + 4 <= packets.size(); i += 4)
    for (int k = 0; k < usbmidi_packet_len(&packets[i]); k++) out.push_back(packets[i + 1 + k]);
  return out;
}

int main() {
  // SysEx of lengths 2..40: the same packets as usbmidi_pack_sysex, and back to the same bytes
  for (int n = 2; n <= 40; n++) {
    Bytes m(n);
    m[0] = 0xF0;
    for (int i = 1; i < n - 1; i++) m[i] = (uint8_t)(i & 0x7F);
    m[n - 1] = 0xF7;
    Bytes want(usbmidi_sysex_bytes(n));
    want.resize(usbmidi_pack_sysex(m.data(), n, want.data()));
    Bytes got = Packets(m);
    CHECK(got == want);
    CHECK(Stream(got) == m);
  }
  // channel messages, running status, program change, real time inside a SysEx
  {
    Bytes in = {0x90, 60, 100, 62, 90, 0xC3, 5, 0xF0, 1, 0xF8, 2, 3, 0xF7, 0xB0, 7, 99};
    Bytes want = {0x09, 0x90, 60, 100, 0x09, 0x90, 62, 90, 0x0C, 0xC3, 5, 0,
                  0x0F, 0xF8, 0, 0, 0x04, 0xF0, 1, 2, 0x06, 3, 0xF7, 0, 0x0B, 0xB0, 7, 99};
    CHECK(Packets(in) == want);
  }
  // stray data bytes before any status are dropped; song position is system common (CIN 3)
  {
    Bytes in = {5, 6, 0xF2, 1, 2, 0xF8};
    Bytes want = {0x03, 0xF2, 1, 2, 0x0F, 0xF8, 0, 0};
    CHECK(Packets(in) == want);
  }
  printf("test_usbmidi_stream: PASS\n");
  return 0;
}
