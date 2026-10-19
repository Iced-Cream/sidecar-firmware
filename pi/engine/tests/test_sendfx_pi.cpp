// pi/engine/tests/test_sendfx_pi.cpp - sendfx_pi (the send FX between the IN completion, render core 2 and the OUT
// fill), one thread standing in for the three: off = no work and OUT untouched; on = the return primes (kPrime
// frames), lands on 2/3 (P1 PRE, or route fx) or 0/1 (P1 POST, or route main) added to what is there, the other channels untouched; a
// dry ring is counted and primes again; the console's sendfx line.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "check.h"
#include "fx/send_fx.h"
#include "sendfx_pi.h"

static std::string g_log;
namespace core {
uint32_t cycles() { static uint32_t c; return c += 1000; }
uint32_t cycles_per_second() { return 54000000; }
uint32_t g_ms;
uint32_t now_ms() { return g_ms; }
void log(const char* fmt, ...) {
  char b[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  g_log += b;
  g_log += "\n";
}
}  // namespace core

using namespace core::fx;

#include <algorithm>
#include <cmath>
#include <set>
static std::set<int64_t> g_sent;   // every stem 0 value as float -> int (the insert's exact passthrough)
static void in_block(int k) {   // 7 frames: stem 0 a 220 Hz sine at 0.1 FS
  int32_t s[7][8] = {};
  for (int f = 0; f < 7; ++f) s[f][0] = (int32_t)(0.1 * 2147483647.0 * std::sin(6.2831853 * 220 * (7 * k + f) / 48000));
  for (int f = 0; f < 7; ++f) g_sent.insert((int64_t)(float)s[f][0]);
  sendfx_pi::FromRytm(s, 7);
}
static void out_block(int32_t (*o)[12]) {
  for (int f = 0; f < 7; ++f)
    for (int c = 0; c < 12; ++c) o[f][c] = 1000 * (c + 1);
  sendfx_pi::AddReturn(o, 7);
}

int main() {
  int32_t o[7][12];
  CHECK(!sendfx_pi::On());
  for (int k = 0; k < 100; ++k) in_block(k), sendfx_pi::Render(), out_block(o);
  for (int c = 0; c < 12; ++c) CHECK(o[6][c] == 1000 * (c + 1));   // off: OUT untouched
  control.knob[0][0] = kComb;
  control.send[0][0] = 127;
  CHECK(sendfx_pi::On());
  bool seen = false;
  int blocks_to_return = -1;
  for (int k = 0; k < 2000; ++k) {
    in_block(k);
    if (k % 2) sendfx_pi::Render(), sendfx_pi::Render();   // core 2 at its own pace (every 14 frames here)
    out_block(o);
    bool moved = o[3][2] != 3000 || o[3][3] != 4000;
    if (moved && blocks_to_return < 0) blocks_to_return = k;
    seen |= moved;
    for (int c = 0; c < 12; ++c)
      if (c != 2 && c != 3) CHECK(o[3][c] == 1000 * (c + 1));
  }
  CHECK(seen && blocks_to_return * 7 >= (int)sendfx_pi::kPrime - 7);
  {   // each FX's own return (the recorder's FX1/FX2): FX 1 alone plays, FX 2's slot stays silent
    float sl[7][4] = {};
    float m1 = 0, m2 = 0;
    for (int k = 0; k < 50; ++k) {
      in_block(k), sendfx_pi::Render();
      for (int f = 0; f < 7; ++f)
        for (int c = 0; c < 12; ++c) o[f][c] = 0;
      sendfx_pi::AddReturn(o, 7, sl);
      for (int f = 0; f < 7; ++f) m1 = std::max(m1, std::fabs(sl[f][0]) + std::fabs(sl[f][1])), m2 = std::max(m2, std::fabs(sl[f][2]) + std::fabs(sl[f][3]));
    }
    CHECK(m1 > 1e-4f && m2 == 0.0f);
  }
  control.route = kRouteMain;
  for (int k = 0; k < 200; ++k) in_block(k), sendfx_pi::Render(), out_block(o);
  CHECK((o[3][0] != 1000 || o[3][1] != 2000) && o[3][2] == 3000 && o[3][3] == 4000);
  // a stalled core 2: the ring runs dry, the fill counts it and leaves OUT as it was
  for (int k = 0; k < 40; ++k) in_block(k), out_block(o);
  CHECK(o[3][0] == 1000 && o[3][1] == 2000);
  g_log.clear();
  CHECK(sendfx_pi::Command("sendfx") && g_log.find("fx1 COMB (2)") != std::string::npos);
  CHECK(g_log.find("underruns 1") != std::string::npos && g_log.find("faults 0") != std::string::npos);
  printf("%s", g_log.c_str());
  {   // the load is busy ticks over the elapsed milliseconds, not a 32-bit tick difference: 2000 renders (1000 ticks each at
      // 54 MHz) in the 5 minutes since the last line (4 wraps of a 32-bit tick count) are 0.012 %, not 4 wraps' worth
    control.route = kRouteSlot;
    core::g_ms += 300000;
    for (int k = 0; k < 2000; ++k) in_block(k), sendfx_pi::Render(), out_block(o);
    g_log.clear();
    CHECK(sendfx_pi::Command("sendfx"));
    CHECK(g_log.find("core 2 load 0.012 percent") != std::string::npos && g_log.find("frames, core 2") != std::string::npos);
    printf("%s", g_log.c_str());
    core::g_ms += 1000;   // a second later with no render: 0 %, and the frames since the last line are 0
    g_log.clear();
    CHECK(sendfx_pi::Command("sendfx") && g_log.find(" 0 frames, core 2 load 0.000 percent") != std::string::npos);
  }
  CHECK(!sendfx_pi::Command("out=main"));
  // the display's reads and its kill button
  sendfx_pi::View v;
  control.send[3][1] = 77;
  sendfx_pi::Read(&v);
  CHECK(strcmp(v.type[0], "COMB") == 0 && v.send[0][0] == 127 && v.send[1][3] == 77);
  g_log.clear();
  CHECK(sendfx_pi::Command("killfx") && g_log.find("kill tails") != std::string::npos);
  // STUTTER (FX 2 here): the stems' sum onto MAIN 0/1 with the route at fx, 2/3 still FX 1's; disengaged = the sum
  control.route = kRouteSlot;   // P1 per slot: FX 1 (COMB) PRE on 2/3, the STUTTER on FX 2 POST on 0/1
  control.knob[0][1] = 0;
  control.knob[1][1] = 127;
  control.knob[1][0] = kStutter;
  sendfx_pi::Clock(0xFA, 0), sendfx_pi::Clock(0xF8, 0);
  for (int k = 0; k < 400; ++k) in_block(k), sendfx_pi::Render(), out_block(o);
  for (int f = 0; f < 7; ++f) {   // every frame one of the stem's values exactly (its own latency), on both sides
    int64_t d0 = (int64_t)o[f][0] - 1000, d1 = (int64_t)o[f][1] - 2000;
    CHECK(d0 == d1 && g_sent.count(d0) && d0 != 0 && o[f][2] != 3000);
  }
  g_log.clear();
  CHECK(sendfx_pi::Command("sendfx") && g_log.find("fx2 STUTTER (15)") != std::string::npos);
  printf("test_sendfx_pi: PASS\n");
  return 0;
}
