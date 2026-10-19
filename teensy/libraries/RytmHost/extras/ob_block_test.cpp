// Host test of ob_block.h (no Teensy needed):
//   c++ -std=c++17 -Wall -I teensy/libraries/RytmHost \
//       teensy/libraries/RytmHost/extras/ob_block_test.cpp -o /tmp/ob_test && /tmp/ob_test
#include "ob_block.h"

#include <cstdio>
#include <cstdlib>

static int fails = 0;
#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++fails;                                                \
    }                                                         \
  } while (0)

// The configuration descriptor the Rytm MKII (OS 1.73, OVERBRIDGE mode) sends,
// as read by rytm-emu's simulated host (dtboot --usb-host, 2026-10-09)
// [measured in the emulator, not yet on hardware]: device 1935:0b48, class
// EF/02/01 (IADs); 6 interfaces: 0 vendor; 1 vendor, alts 1/2/3 interrupt IN
// 0x83 32/88/368 B bInterval 1; 2 the same for OUT 0x03; 3 vendor, bulk OUT
// 0x02 512 B + interrupt IN 0x82 64 B; 4 audio control; 5 MIDI streaming,
// bulk 0x01 / 0x81 512 B.
static const uint8_t kRytmConfig[263] = {
    0x09, 0x02, 0x07, 0x01, 0x06, 0x01, 0x00, 0xc0, 0x00, 0x08, 0x0b, 0x00, 0x04, 0xff, 0x00, 0x00, 0x00, 0x09,
    0x04, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x09, 0x04, 0x01, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x09,
    0x04, 0x01, 0x01, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x83, 0x03, 0x20, 0x00, 0x01, 0x09, 0x04, 0x01,
    0x02, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x83, 0x03, 0x58, 0x00, 0x01, 0x09, 0x04, 0x01, 0x03, 0x01,
    0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x83, 0x03, 0x70, 0x01, 0x01, 0x09, 0x04, 0x02, 0x00, 0x00, 0xff, 0x00,
    0x00, 0x00, 0x09, 0x04, 0x02, 0x01, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x03, 0x03, 0x20, 0x00, 0x01,
    0x09, 0x04, 0x02, 0x02, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x03, 0x03, 0x58, 0x00, 0x01, 0x09, 0x04,
    0x02, 0x03, 0x01, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x03, 0x03, 0x70, 0x01, 0x01, 0x09, 0x04, 0x03, 0x00,
    0x02, 0xff, 0x00, 0x00, 0x00, 0x07, 0x05, 0x02, 0x02, 0x00, 0x02, 0x00, 0x07, 0x05, 0x82, 0x03, 0x40, 0x00,
    0x01, 0x08, 0x0b, 0x04, 0x02, 0x01, 0x01, 0x00, 0x00, 0x09, 0x04, 0x04, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x05, 0x09, 0x04, 0x05, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00, 0x06, 0x24, 0x02, 0x01, 0x01, 0x00, 0x06, 0x24, 0x02, 0x02, 0x02,
    0x00, 0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00, 0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01,
    0x00, 0x09, 0x05, 0x01, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x05, 0x25, 0x01, 0x01, 0x01, 0x09, 0x05, 0x81,
    0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x05, 0x25, 0x01, 0x01, 0x03};

int main() {
  // descriptor scan: the whole configuration, and the tail USBHost_t36 gives
  // claim() at interface 1 alt 0 (offset 26)
  ob::Layout l;
  CHECK(ob::find_layout(kRytmConfig, sizeof kRytmConfig, ob::kAlt, &l));
  CHECK(l.in.mps == 368 && l.out.mps == 368 && l.in.interval == 1 && l.out.interval == 1);
  CHECK(l.in_channels == 12 && l.out_channels == 12);
  CHECK(l.midi_iface == 5);
  CHECK(l.interfaces == 6);
  const uint8_t* if1 = kRytmConfig + 26;
  CHECK(if1[1] == 4 && if1[2] == 1 && if1[3] == 0);
  CHECK(ob::find_layout(if1, sizeof kRytmConfig - 26, ob::kAlt, &l) && l.in_channels == 12);
  CHECK(ob::find_layout(kRytmConfig, sizeof kRytmConfig, 2, &l) && l.in_channels == 2);   // alt 2: 88 B
  CHECK(!ob::find_layout(kRytmConfig, sizeof kRytmConfig, 4, &l));
  CHECK(ob::channels_for(32) == 0 && ob::channels_for(60) == 1 && ob::channels_for(367) == 0);

  // host -> device block: header, counter, voice v on its channel, x 65536
  uint8_t blk[368];
  ob::write_header(blk, 0x1234);
  CHECK(blk[0] == 0x07 && blk[1] == 0xFF && blk[2] == 0x12 && blk[3] == 0x34);
  for (int i = 4; i < 32; ++i) CHECK(blk[i] == 0);
  int16_t v[8] = {1, 2, 3, 4, 5, 6, 7, -32768};
  ob::put_voice_frame(blk, 12, 6, v, ob::kVoiceToRytm, -1);
  for (int i = 0; i < 8; ++i) CHECK(ob::block_sample(blk, 12, 6, ob::kVoiceToRytm[i]) == (int32_t)v[i] * 65536);
  CHECK(ob::block_sample(blk, 12, 6, 0) == 0 && ob::block_sample(blk, 12, 6, 3) == 0);
  // big-endian on the wire: frame 6, channel 4 (voice 0 = BD) at 32 + 4 * (6 * 12 + 4)
  const uint8_t* s = blk + 32 + 4 * (6 * 12 + 4);
  CHECK(s[0] == 0x00 && s[1] == 0x01 && s[2] == 0x00 && s[3] == 0x00);
  // voice 7 (RS/CP's voice) on channel 6, -32768 -> 0x80000000
  s = blk + 32 + 4 * (6 * 12 + 6);
  CHECK(s[0] == 0x80 && s[1] == 0 && s[2] == 0 && s[3] == 0);
  ob::put_voice_frame(blk, 12, 0, v, ob::kVoiceToRytm, 0);   // mirror voice 0 to MAIN L/R
  CHECK(ob::block_sample(blk, 12, 0, 0) == 65536 && ob::block_sample(blk, 12, 0, 1) == 65536);
  ob::put_voice_frame(blk, 12, 0, nullptr, ob::kVoiceToRytm, 0);
  for (int c = 0; c < 12; ++c) CHECK(ob::block_sample(blk, 12, 0, c) == 0);

  // device -> host
  uint8_t in[368] = {0x07, 0x00, 0x00, 0x0e};
  CHECK(ob::block_from_device(in) && !ob::block_from_device(blk));
  CHECK(ob::block_counter(in) == 14);

  // the voice tables are permutations: 4..11 to the Rytm, 2..9 from it
  int seen_to = 0, seen_from = 0;
  for (int i = 0; i < 8; ++i) {
    seen_to |= 1 << ob::kVoiceToRytm[i];
    seen_from |= 1 << ob::kVoiceFromRytm[i];
  }
  CHECK(seen_to == 0xFF0 && seen_from == 0x3FC);

  // main only (kVoiceToNone, mirror 0): slot 0 on MAIN L/R, every other channel silent
  {
    uint8_t b[368];
    const int16_t fr[8] = {0x1234, 1, 2, 3, 4, 5, 6, 7};
    ob::put_voice_frame(b, 12, 2, fr, ob::kVoiceToNone, 0);
    CHECK(ob::block_sample(b, 12, 2, 0) == 0x12340000 && ob::block_sample(b, 12, 2, 1) == 0x12340000);
    for (int c = 2; c < 12; ++c) CHECK(ob::block_sample(b, 12, 2, c) == 0);
  }

  std::printf(fails ? "%d failures\n" : "ob_block: all checks passed\n", fails);
  return fails ? 1 : 0;
}
