// core/tests/test_route.cpp - the audio route (core/route): ROUTE / MOVE / UNROUTE / GAIN through the host link
// (core/kv/link, answers through hooks.to_rytm), the refusals (MAIN as a source, voice loops, full, wrong owner), the
// ramps, the lock service reaching a point's gain (target 4), and the HELLO closing every point.
#include <cmath>
#include <cstring>
#include <vector>

#include "check.h"
#include "clock/clock.h"
#include "hooks.h"
#include "kv/link.h"
#include "locks/locks.h"
#include "route/route.h"

using namespace core;

static std::vector<std::vector<uint8_t>> g_out;
static void capture(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }

static std::vector<uint8_t> msg(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x51, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static bool send(std::vector<uint8_t> body) {
  auto m = msg(body);
  return kv::link::from_host(m.data(), (int)m.size());
}
// the answer to the last ROUTE / MOVE: point (>= 0) or -code
static int answer() {
  g_out.clear();
  kv::link::host_step(8);
  CHECK(g_out.size() == 1);
  const auto& a = g_out[0];
  CHECK(a.size() == 11 && a[4] == 0x51 && a[5] == 1 && a[10] == 0xF7);
  if (a[6] == 0x60) return a[9];
  CHECK(a[6] == 0x61);
  return -a[9];
}
static int rt(int seq, int owner, int src, int dst, int gain) {
  send({0x20, (uint8_t)seq, (uint8_t)owner, (uint8_t)src, (uint8_t)dst, (uint8_t)(gain >> 7), (uint8_t)(gain & 0x7F)});
  return answer();
}

// render n frames with every IN channel at 0.5 (IN c: 0.5 + c / 100, to tell them apart); returns OUT
static float g_in[12][1024], g_out_buf[12][1024];
static void render(int n) {
  const float* in[12];
  float* out[12];
  for (int c = 0; c < 12; ++c) {
    for (int f = 0; f < n; ++f) g_in[c][f] = 0.5f + c / 100.0f, g_out_buf[c][f] = 0.0f;
    in[c] = g_in[c];
    out[c] = g_out_buf[c];
  }
  route::process(in, out, n);
}
// render n frames in pieces the buffers hold (the last piece's OUT stays in g_out_buf)
static void advance(int64_t n) {
  while (n > 0) { int k = n > 1024 ? 1024 : (int)n; render(k); n -= k; }
}
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
  hooks.to_rytm = capture;
  CHECK(kv::link::attach_generic());

  // nothing open: no work, OUT untouched
  CHECK(!route::any());
  render(64);
  for (int c = 0; c < 12; ++c) CHECK(g_out_buf[c][63] == 0.0f);

  // refusals: MAIN as a source, a channel past 11, a bad length
  CHECK(rt(1, 6, 0, 0, 8192) == -route::kBad);
  CHECK(rt(2, 6, 1, 4, 8192) == -route::kBad);
  CHECK(rt(3, 6, 3, 12, 8192) == -route::kBad);
  send({0x20, 4, 6, 3});
  CHECK(answer() == -route::kBad);
  CHECK(!route::any());

  // SD stem (IN 3) -> MAIN L (OUT 0) at unity: point 0, ramps in over kRamp frames, then 0.53 exactly
  CHECK(rt(5, 6, 3, 0, 8192) == 0);
  CHECK(route::any() && route::point(0).open && route::point(0).src == 3 && route::point(0).dst == 0);
  render(route::kRamp / 2);
  CHECK(g_out_buf[0][0] > 0.0f && g_out_buf[0][0] < 0.01f);   // the first frame of the ramp
  render(route::kRamp);
  CHECK(near(g_out_buf[0][route::kRamp - 1], 0.53f));
  for (int c = 1; c < 12; ++c) CHECK(g_out_buf[c][10] == 0.0f);

  // voice loops: SD stem -> BD input (OUT 4) is fine; BD stem (IN 2) -> SD input (OUT 5) closes the loop; SD -> SD too
  CHECK(rt(6, 6, 3, 4, 8192) == 1);
  CHECK(rt(7, 6, 2, 5, 8192) == -route::kLoop);
  CHECK(rt(8, 6, 3, 5, 8192) == -route::kLoop);
  // through a third voice: BD stem -> RS input (OUT 6) fine, then RS stem (IN 4) -> SD input closes SD>BD>RS>SD
  CHECK(rt(9, 6, 2, 6, 8192) == 2);
  CHECK(rt(10, 6, 4, 5, 8192) == -route::kLoop);
  // AUDIO IN (IN 10) -> SD input: no loop
  CHECK(rt(11, 7, 10, 5, 4096) == 3);

  // gain through the lock service: a set on target 4, its first param = point 0; base 4096 = half
  auto lk = [](std::vector<uint8_t> body) { auto m = msg(body); return kv::link::from_host(m.data(), (int)m.size()); };
  CHECK(lk({0x10, 6, 1, route::kTarget, 0, 1}));      // set 6: track 1 (SD), target 4, param 0, n 1
  CHECK(lk({0x11, 6, 0, 1, 4096 >> 7, 4096 & 0x7F}));   // base half
  CHECK(route::point(0).gain == 4096);
  render(route::kRamp + 8);
  CHECK(near(g_out_buf[0][route::kRamp + 7], 0.265f));
  // a lock of 0 on step 2: played on the step's tick, the point fades to silence
  CHECK(lk({0x12, 6, 2, 0, 0, 0}));
  locks::tick(1, 2, true);
  CHECK(route::point(0).gain == 0);
  render(route::kRamp + 8);
  CHECK(g_out_buf[0][route::kRamp + 7] == 0.0f);
  locks::tick(1, 3, true);   // a step without a lock: back to the base
  CHECK(route::point(0).gain == 4096);
  render(route::kRamp + 8);
  CHECK(near(g_out_buf[0][route::kRamp + 7], 0.265f));

  // stamped gains (the lock sink's, by the tick's block): the Rytm's block b maps to frames by the earliest a tick
  // arrived (frames - 32 b) plus the lead (kLead = 144). A tick arriving now for block b0: lands 144 frames on
  {
    int64_t now = route::frames();
    int64_t b0 = now / 32, o = now - 32 * b0;
    route::stamp(b0);
    CHECK(route::set_gain(0, 0));
    route::stamp(-1);
    CHECK(route::stats().timed == 1 && route::stats().late == 0 && route::stats().lead == route::kLead);
    render(route::kLead / 2);
    CHECK(g_out_buf[0][route::kLead / 2 - 1] > 0.1f && g_out_buf[0][route::kLead / 2 - 1] < 0.16f);   // half way
    render(route::kLead / 2);
    CHECK(near(g_out_buf[0][route::kLead / 2 - 1], 0.0f));   // landed on its frame
    // a lead longer than a ramp (locklead=400): a tick on time for block b1 lands 400 frames on; the ramp waits 160
    // frames, then takes kRamp
    route::set_lead(400);
    int64_t b1 = b0 + 40;
    advance(32 * b1 + o - route::frames());
    route::stamp(b1);
    CHECK(route::set_gain(0, 4096));
    route::stamp(-1);
    render(400 - route::kRamp);
    CHECK(g_out_buf[0][400 - route::kRamp - 1] == 0.0f);   // still waiting
    render(route::kRamp);
    CHECK(near(g_out_buf[0][route::kRamp - 1], 0.265f));   // landed on its frame
    CHECK(near(g_out_buf[0][route::kRamp / 2 - 1], 0.1325f));
    route::set_lead(route::kLead);
    // a tick arriving 200 frames later than the earliest: due 56 frames ago, so it ramps over kMinRamp, counted late
    advance(32 * (b0 + 80) + o + 200 - route::frames());
    route::stamp(b0 + 80);
    CHECK(route::set_gain(0, 0));
    route::stamp(-1);
    CHECK(route::stats().spread == 200);
    render(route::kMinRamp);
    CHECK(route::stats().late == 1);   // (counted where it lands, on the render side)
    CHECK(near(g_out_buf[0][route::kMinRamp - 1], 0.0f));
    // a block far off the estimate (a new stream): the estimate starts again from this tick
    uint32_t rs = route::stats().resyncs;
    route::stamp(b0 + 100000);
    CHECK(route::set_gain(0, 4096));
    route::stamp(-1);
    CHECK(route::stats().resyncs == rs + 1);
    render(route::kRamp + 8);   // (due kLead from now: a shortened ramp)
    CHECK(near(g_out_buf[0][route::kRamp + 7], 0.265f));
    // GAIN (the op) is never stamped, even inside a tick
    route::stamp(b0 + 100100);
    send({0x23, 6, 0, 4096 >> 7, 4096 & 0x7F});
    route::stamp(-1);
    CHECK(route::stats().timed == 4);
  }

  // MOVE: wrong owner refused; point 0 to MAIN R (OUT 1): fades out of 0, then into 1
  send({0x21, 12, 9, 0, 3, 1});
  CHECK(answer() == -route::kBad);
  send({0x21, 13, 6, 0, 3, 1});
  CHECK(answer() == 0);
  CHECK(route::point(0).dst == 1);
  render(3 * route::kRamp);
  CHECK(g_out_buf[0][3 * route::kRamp - 1] == 0.0f);
  CHECK(near(g_out_buf[1][3 * route::kRamp - 1], 0.265f));

  // GAIN directly (owner 7's point 3, AUDIO IN L -> SD input): 0 -> silent; another owner's GAIN ignored
  send({0x23, 6, 3, 0, 0});
  CHECK(route::point(3).gain == 4096);
  send({0x23, 7, 3, 0, 0});
  CHECK(route::point(3).gain == 0);

  // full: 16 points, the 17th refused
  int k = 0;
  while (rt(20 + k, 9, 11, 2, 8192) >= 0) ++k;
  CHECK(k == route::kPoints - 4);
  CHECK(rt(50, 9, 11, 2, 8192) == -route::kFull);

  // UNROUTE every point of owner 9, then point 1 of owner 6
  send({0x22, 9, 0x7F});
  CHECK(route::stats().open == 4);
  send({0x22, 6, 1});
  CHECK(!route::point(1).open && route::stats().open == 3);
  render(route::kRamp + 1);

  // the companion's HELLO closes everything; the points fade out, then no work is left
  send({0x00, 0, 0, 0, 0, 1});
  CHECK(route::stats().open == 0);
  CHECK(route::any());   // still fading
  render(route::kRamp + 1);
  CHECK(!route::any());
  render(16);
  for (int c = 0; c < 12; ++c) CHECK(g_out_buf[c][15] == 0.0f);
  // a gain for a closed point is refused (the lock sink's too)
  CHECK(!route::set_gain(0, 8192));

  printf("test_route: OK (%lu routed, %lu refused)\n", (unsigned long)route::stats().routed,
         (unsigned long)route::stats().refused);
  return 0;
}
