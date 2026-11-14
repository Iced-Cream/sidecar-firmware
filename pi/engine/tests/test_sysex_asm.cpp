// pi/engine/tests/test_sysex_asm.cpp - whole SysEx from the second box's byte stream (sysex_asm.h): a hello and a report
// come out whole, realtime bytes inside are skipped, note bytes around are ignored, a broken or too long message is
// dropped and counted, and the next one after it is whole again.
#include <cstring>
#include <vector>

#include "check.h"
#include "sysex_asm.h"

template <unsigned N>
static std::vector<std::vector<uint8_t>> feed(SysExAsm<N>& a, const std::vector<uint8_t>& in) {
  std::vector<std::vector<uint8_t>> out;
  for (uint8_t b : in)
    if (unsigned n = a.feed(b)) out.emplace_back(a.data(), a.data() + n);
  return out;
}

int main() {
  const std::vector<uint8_t> hello = {0xF0, 0x7D, 0x52, 0x54, 0x4F, 1, 0, 3, 1, 'A', '4', 0xF7};
  const std::vector<uint8_t> off = {0xF0, 0x7D, 0x52, 0x54, 0x11, 2, 0xF7};
  {
    SysExAsm<64> a;
    std::vector<uint8_t> in = {0x90, 60, 100};   // a note before: not ours
    in.insert(in.end(), hello.begin(), hello.end());
    in.insert(in.end(), off.begin(), off.end());
    auto m = feed(a, in);
    CHECK(m.size() == 2 && m[0] == hello && m[1] == off && a.bad() == 0);
  }
  {
    SysExAsm<64> a;   // a clock tick and active sensing inside the hello
    std::vector<uint8_t> in(hello.begin(), hello.begin() + 5);
    in.push_back(0xF8);
    in.insert(in.end(), hello.begin() + 5, hello.end() - 1);
    in.push_back(0xFE);
    in.push_back(0xF7);
    auto m = feed(a, in);
    CHECK(m.size() == 1 && m[0] == hello && a.bad() == 0);
  }
  {
    SysExAsm<64> a;   // broken by a status byte, cut by a new F0: both dropped, counted; the next whole
    std::vector<uint8_t> in = {0xF0, 0x7D, 0x52, 0x90, 1, 2, 0xF0, 0x7D, 0x52};
    in.insert(in.end(), off.begin(), off.end());
    auto m = feed(a, in);
    CHECK(m.size() == 1 && m[0] == off && a.bad() == 2);
  }
  {
    SysExAsm<8> a;   // longer than the room: dropped whole up to its F7, counted once; the next fits
    std::vector<uint8_t> in = {0xF0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0xF7};
    in.insert(in.end(), off.begin(), off.end());
    auto m = feed(a, in);
    CHECK(m.size() == 1 && m[0] == off && a.bad() == 1);
  }
  printf("test_sysex_asm: ok\n");
  return 0;
}
