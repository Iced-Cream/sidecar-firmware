// pi/engine/tests/test_masterfx_pi.cpp - masterfx_pi (the OUT fill's side of the master chain, render core 2's), one thread
// standing in for both, as test_sendfx_pi: nothing set = not a sample touched; bypassed = bit for bit; on = 0/1 and 2/3 go
// through the chain and come back on 0/1; the crossfades have no click; the latency on POST; a dry ring; SRC MIX.
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fx/insert.h"
#include "fx/limiter.h"
#include "fx/master.h"
#include "fx/out.h"
#include "fx/out_measure.h"
#include "fx/send_fx.h"
#include "masterfx_pi.h"

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
extern "C" int core_vsnprintf(char* out, size_t n, const char* fmt, va_list ap) { return vsnprintf(out, n, fmt, ap); }
extern "C" int core_snprintf(char* out, size_t n, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(out, n, fmt, ap);
  va_end(ap);
  return r;
}

using namespace core;

static const int kLook = fx::Limiter::kLook;
static unsigned kFill = 21;   // frames a fill: 7 x obblocks (the default 1: see latency())
static long g_t = 0;
typedef void (*Gen)(long t, int32_t* ch);
static void post_sine(long t, int32_t* ch) {
  ch[0] = (int32_t)(0.2 * 2147483648.0 * std::sin(2 * M_PI * 220.0 * (double)t / 48000.0));
  ch[1] = (int32_t)(0.2 * 2147483648.0 * std::sin(2 * M_PI * 331.0 * (double)t / 48000.0));
}
static void all_sine(long t, int32_t* ch) {
  post_sine(t, ch);
  ch[2] = ch[0] / 2, ch[3] = ch[1] / 3;
  for (int c = 4; c < 12; ++c) ch[c] = (c - 3) * 1000000 + (int32_t)t;
}
// one fill: n frames from g, Process, the render core's turn (every other fill, as a slower core 2 would)
static void fill(Gen g, int32_t (*f)[12], unsigned n) {
  memset(f, 0, sizeof(int32_t) * 12 * n);
  for (unsigned i = 0; i < n; ++i) g(g_t + i, f[i]);
  masterfx_pi::Process(f, n);
  masterfx_pi::Render();
  g_t += n;
}
static void off() {
  ins::reset();
  masterfx_pi::Reset();
  int32_t f[64][12];
  for (int i = 0; i < 8; ++i) fill([](long, int32_t*) {}, f, kFill);
  masterfx_pi::Reset();
  CHECK(masterfx_pi::Wet() == 0.0f);
}
// an EQ, flat: a chain that passes the signal
static void flat_chain() {
  ins::set_type(master::chain(0), master::slot(0), ins::kEq);
  CHECK(master::on());
}

static void nothing_set_touches_nothing() {
  off();
  int32_t f[64][12], want[12];
  for (int k = 0; k < 400; ++k) {
    long t = g_t;
    fill(all_sine, f, kFill);
    for (unsigned i = 0; i < kFill; ++i) {
      memset(want, 0, sizeof want);
      all_sine(t + i, want);
      CHECK(!memcmp(f[i], want, sizeof want));
    }
  }
  CHECK(masterfx_pi::Read().frames == 0 || masterfx_pi::Wet() == 0.0f);
  printf("test_masterfx_pi: nothing set: every channel of every frame bit for bit, core 2 never called on\n");
}

static void bypass_bit_exact() {
  off();
  flat_chain();
  ins::set_type(master::chain(2), master::slot(2), ins::kMSpace);
  ins::set_type(master::chain(3), master::slot(3), ins::kComp);
  master::set_byp(1);
  int32_t f[64][12], want[12];
  long n = 0;
  uint32_t fr0 = masterfx_pi::Read().frames;
  for (int k = 0; k < 2000; ++k) {
    long t = g_t;
    fill(all_sine, f, kFill);
    for (unsigned i = 0; i < kFill; ++i) {
      memset(want, 0, sizeof want);
      all_sine(t + i, want);
      CHECK(!memcmp(f[i], want, sizeof want));
      ++n;
    }
  }
  CHECK(masterfx_pi::Read().frames - fr0 > 40000);   // the chain went on running behind it
  master::set_byp(0);
  for (int k = 0; k < 40; ++k) fill(all_sine, f, kFill);
  CHECK(masterfx_pi::Wet() == 1.0f);
  master::set_byp(1);
  for (int k = 0; k < 40; ++k) fill(all_sine, f, kFill);   // back to bypass: faded, then bit for bit again
  CHECK(masterfx_pi::Wet() == 0.0f);
  for (int k = 0; k < 200; ++k) {
    long t = g_t;
    fill(all_sine, f, kFill);
    for (unsigned i = 0; i < kFill; ++i) {
      memset(want, 0, sizeof want);
      all_sine(t + i, want);
      CHECK(!memcmp(f[i], want, sizeof want));
    }
  }
  printf("test_masterfx_pi: bypass: %ld frames of all 12 channels bit for bit with the chain running, again after on and off\n", n);
}

// the latency on POST: a click through a flat chain, and what it does to a sine
static void latency() {
  off();
  flat_chain();
  static std::vector<float> in, outp;
  int32_t f[64][12];
  for (int k = 0; k < 100; ++k) fill([](long, int32_t*) {}, f, kFill);   // primed, faded in
  CHECK(masterfx_pi::Wet() == 1.0f);
  long at = g_t + 500;
  static long s_at;
  s_at = at;
  long found = -1;
  for (int k = 0; k < 100 && found < 0; ++k) {
    long t = g_t;
    fill([](long u, int32_t* ch) { if (u == s_at) ch[0] = 1 << 28; }, f, kFill);
    for (unsigned i = 0; i < kFill; ++i)
      if (std::abs(f[i][0]) > (1 << 26)) found = t + i;
  }
  CHECK(found >= 0);
  long lat = found - at;
  printf("test_masterfx_pi: added latency on POST: %ld frames (%.2f ms) at %u-frame fills (obblocks 3), kPrime %u\n", lat, lat / 48.0, kFill, masterfx_pi::kPrime);
  CHECK(lat >= (long)masterfx_pi::kPrime + kLook && lat <= (long)masterfx_pi::kPrime + kLook + 2 * (long)kFill);
  // the default URB, 7 frames (ob_usb.cpp obblocks 1)
  off();
  flat_chain();
  kFill = 7;
  for (int k = 0; k < 300; ++k) fill([](long, int32_t*) {}, f, kFill);
  CHECK(masterfx_pi::Wet() == 1.0f);
  s_at = g_t + 50;
  found = -1;
  for (int k = 0; k < 300 && found < 0; ++k) {
    long t = g_t;
    fill([](long u, int32_t* ch) { if (u == s_at) ch[0] = 1 << 28; }, f, kFill);
    for (unsigned i = 0; i < kFill; ++i)
      if (std::abs(f[i][0]) > (1 << 26)) found = t + i;
  }
  CHECK(found >= 0);
  lat = found - s_at;
  printf("test_masterfx_pi: added latency on POST: %ld frames (%.2f ms) at 7-frame fills (the default), the limiter's %d in it, the chain adding none\n", lat, lat / 48.0, kLook);
  CHECK(lat >= (long)masterfx_pi::kPrime + kLook && lat <= (long)masterfx_pi::kPrime + kLook + 2 * 7);
  kFill = 21;
  // bypassed: none
  master::set_byp(1);
  for (int k = 0; k < 40; ++k) fill([](long, int32_t*) {}, f, kFill);
  s_at = g_t + 5;
  long t = g_t;
  fill([](long u, int32_t* ch) { if (u == s_at) ch[0] = 1 << 28; }, f, kFill);
  CHECK(f[s_at - t][0] == 1 << 28);
}

// PRE moves onto the chain, at its level on the mix; TRK channels 4..11 are not touched
static void returns_go_through() {
  off();
  ins::set_type(master::chain(0), master::slot(0), ins::kFilter);   // an LP at 500 Hz: anything through it is seen
  ins::set_param(master::chain(0), master::slot(0), 0, 72);
  ins::set_param(master::chain(0), master::slot(0), 1, 0);
  int32_t f[64][12];
  long t0 = g_t;
  for (int k = 0; k < 100; ++k) fill(all_sine, f, kFill);
  (void)t0;
  double pre = 0, l = 0;
  for (int k = 0; k < 200; ++k) {
    fill([](long t, int32_t* ch) {
      ch[0] = 0;
      ch[2] = (int32_t)(0.1 * 2147483648.0 * std::sin(2 * M_PI * 100.0 * (double)t / 48000.0));
      ch[5] = 77;
    }, f, kFill);
    for (unsigned i = 0; i < kFill; ++i) {
      pre = std::max(pre, (double)std::abs((double)f[i][2]));
      if (k > 20) l = std::max(l, (double)std::abs((double)f[i][0]));   // (the sine before it gone from the way back)
      CHECK(f[i][5] == 77);
    }
  }
  CHECK(pre == 0);
  double g = l / (0.1 * 2147483648.0);
  printf("test_masterfx_pi: 2/3 are moved onto the chain (gone from 2/3), on 0/1 at x %.3f; channels 4..11 untouched\n", g);
  CHECK(std::fabs(g - masterfx_pi::kPreOnPost) < 0.1);
}

// switched every 100 ms under a sine: the largest step between samples
static void no_click() {
  off();
  flat_chain();
  int32_t f[64][12];
  for (int k = 0; k < 100; ++k) fill(post_sine, f, kFill);
  double mx = 0, prev = 0;
  bool have = false;
  for (int k = 0; k < 6000; ++k) {
    if (k % 228 == 0) master::set_byp(!master::control.byp);
    fill(post_sine, f, kFill);
    for (unsigned i = 0; i < kFill; ++i) {
      double x = f[i][0] / 2147483648.0;
      if (have) mx = std::max(mx, std::fabs(x - prev));
      prev = x, have = true;
    }
  }
  double own = 0.2 * 2 * M_PI * 220.0 / 48000.0;
  printf("test_masterfx_pi: bypass switched every 100 ms for 13 s: largest step %.5f, the sine's own %.5f\n", mx, own);
  CHECK(mx < own * 1.6);
  // a slot's TYPE changed under it, and the kit's chain gone
  off();
  flat_chain();
  for (int k = 0; k < 100; ++k) fill(post_sine, f, kFill);
  have = false, mx = 0;
  for (int k = 0; k < 3000; ++k) {
    if (k % 300 == 100) ins::set_type(master::chain(1), master::slot(1), k % 600 == 100 ? ins::kMChorus : ins::kOff);
    fill(post_sine, f, kFill);
    for (unsigned i = 0; i < kFill; ++i) {
      double x = f[i][0] / 2147483648.0;
      if (have) mx = std::max(mx, std::fabs(x - prev));
      prev = x, have = true;
    }
  }
  printf("test_masterfx_pi: a send TYPE in and out of a slot: largest step %.5f\n", mx);
  CHECK(mx < own * 3);
  ins::clear();   // a kit without master FX: fades out, stops
  for (int k = 0; k < 60; ++k) fill(post_sine, f, kFill);
  CHECK(masterfx_pi::Wet() == 0.0f);
  int32_t want[12];
  long t = g_t;
  fill(post_sine, f, kFill);
  memset(want, 0, sizeof want);
  post_sine(t, want);
  CHECK(!memcmp(f[0], want, sizeof want));
  printf("test_masterfx_pi: a kit change to no chain: fades out, then bit for bit\n");
}

// stereo=measure while a chain plays: the chain stands aside, so the measurement reads the plain return
static void measure_stands_aside() {
  off();
  flat_chain();
  int32_t f[64][12];
  for (int k = 0; k < 40; ++k) fill(post_sine, f, kFill);
  CHECK(masterfx_pi::Wet() == 1.0f);
  char o[120];
  CHECK(out::command("stereo=measure", o, sizeof o));
  CHECK(out::meas::active());
  for (int k = 0; k < 40; ++k) fill(post_sine, f, kFill);
  CHECK(masterfx_pi::Wet() == 0.0f);
  printf("test_masterfx_pi: stereo=measure with a chain on: the chain out, the plain return measured\n");
  out::meas::reset();
  out::reset();
}

// a render core that does not keep up: counted, primes again, and nothing runs away
static void dry_ring() {
  off();
  flat_chain();
  int32_t f[64][12];
  for (int k = 0; k < 100; ++k) fill(post_sine, f, kFill);
  uint32_t u0 = masterfx_pi::Read().under;
  for (int k = 0; k < 12; ++k) {   // core 2 stalled
    memset(f, 0, sizeof(int32_t) * 12 * kFill);
    masterfx_pi::Process(f, kFill);
  }
  CHECK(masterfx_pi::Read().under > u0);
  for (int k = 0; k < 40; ++k) masterfx_pi::Render(), fill(post_sine, f, kFill);
  printf("test_masterfx_pi: a stalled render core: counted (%u), primes again\n", masterfx_pi::Read().under - u0);
}

// SRC MIX with the Rytm's report: pans by its law, a stem on main left out, a move without a click; the report gone: centred
static void src_mix_report() {
  off();
  master::set_src(master::kMix);
  master::set_trim(0);
  int32_t f[64][12], s[64][8];
  double la = 0, ra = 0, step = 0, prev = 0;
  auto block = [&](bool measure) {
    for (unsigned i = 0; i < kFill; ++i) {
      memset(s[i], 0, sizeof s[i]);
      double x = 0.04 * 2147483648.0 * std::sin(2 * M_PI * 220.0 * (double)(g_t + i) / 48000.0);
      s[i][0] = (int32_t)x;   // BD
      s[i][6] = (int32_t)x;   // CH/OH: hot, trimmed
    }
    masterfx_pi::Stems(s, kFill);
    memset(f, 0, sizeof f);
    masterfx_pi::Process(f, kFill);
    masterfx_pi::Render();
    g_t += kFill;
    if (measure)
      for (unsigned i = 0; i < kFill; ++i) {
        double l = f[i][0] / 2147483648.0;
        la = std::max(la, std::fabs(l)), ra = std::max(ra, std::fabs(f[i][1] / 2147483648.0));
        step = std::max(step, std::fabs(l - prev)), prev = l;
      }
  };
  auto rep = [&](int bd, int ch, int route) {
    uint8_t m[16] = {0xF0, 0x7D, 0x52, 0x54, 0x50, (uint8_t)bd, 64, 64, 64, 64, 64, (uint8_t)ch, 64, (uint8_t)(route >> 7), (uint8_t)(route & 0x7F), 0xF7};
    CHECK(master::report(m, 16));
  };
  auto run = [&](int settle) {
    for (int k = 0; k < settle; ++k) block(false);
    la = ra = 0;
    for (int k = 0; k < 100; ++k) block(true);
  };
  const double g = 0.04 * masterfx_pi::kStemOnPost;   // under the ceiling's knee, hard-panned too
  // BD's route bit 3, CH/OH's bit 4: both off main
  rep(64, 64, 0xFF);
  run(100);
  CHECK(std::fabs(la - g * (1 + masterfx_pi::kStemHotTrim)) < 0.004 && std::fabs(la - ra) < 1e-5);
  rep(0, 64, 0xFF);       // BD hard left: all of it on L, 6 dB over a centre side
  run(100);
  CHECK(std::fabs(la - g * (2 + masterfx_pi::kStemHotTrim)) < 0.004);
  CHECK(std::fabs(ra - g * masterfx_pi::kStemHotTrim) < 0.004);
  rep(127, 64, 0xFF);     // hard right
  run(100);
  CHECK(std::fabs(ra - g * (2 + masterfx_pi::kStemHotTrim)) < 0.004 && std::fabs(la - g * masterfx_pi::kStemHotTrim) < 0.004);
  rep(64, 64, 0xFF & ~(1 << 3));   // BD back on main: left out, CH/OH stays
  run(100);
  CHECK(std::fabs(la - g * masterfx_pi::kStemHotTrim) < 0.004);
  CHECK(master::stems_on_main() == 1);
  rep(64, 64, 0xFF & ~((1 << 3) | (1 << 4)));
  run(100);
  CHECK(la < 1e-6 && ra < 1e-6);   // nothing doubled: all still on main
  rep(0, 64, 0xFF);       // a jump, hard left and off main: no click
  la = ra = step = 0, prev = f[kFill - 1][0] / 2147483648.0;
  for (int k = 0; k < 40; ++k) block(true);
  CHECK(step < g * (2 * M_PI * 220.0 / 48000.0 * 3.1 + 2.0 / masterfx_pi::kGainFrames * 2));   // the sine's own slope at its largest + the ramp's
  master::link_lost();    // the Rytm gone: back to the centred sum
  CHECK(!master::control.known);
  run(100);
  CHECK(std::fabs(la - ra) < 1e-5);
  master::set_src(master::kRet);
  for (int k = 0; k < 60; ++k) block(false);
  masterfx_pi::Reset();
  printf("test_masterfx_pi: SRC MIX with the Rytm's report: its pan law, a stem on main left out, no click, the report gone = centred\n");
}

// SRC MIX: the stems' sum on 0/1, mono, in and out without a click; the chain takes it; bypass keeps the dry sum
static void src_mix() {
  off();
  CHECK(!masterfx_pi::WantStems());
  master::set_src(master::kMix);
  master::set_trim(0);
  CHECK(masterfx_pi::WantStems());
  int32_t f[64][12], s[64][8];
  auto block = [&](bool in) {
    for (unsigned i = 0; i < kFill; ++i) {
      memset(s[i], 0, sizeof s[i]);
      double x = 0.1 * 2147483648.0 * std::sin(2 * M_PI * 220.0 * (double)(g_t + i) / 48000.0);
      s[i][0] = (int32_t)x, s[i][3] = (int32_t)(x / 2);
    }
    if (in) masterfx_pi::Stems(s, kFill);
    memset(f, 0, sizeof f);
    masterfx_pi::Process(f, kFill);
    masterfx_pi::Render();
    g_t += kFill;
  };
  for (int k = 0; k < 100; ++k) block(true);
  double mx = 0, st = 0, prev = 0;
  for (int k = 0; k < 300; ++k) {
    block(true);
    for (unsigned i = 0; i < kFill; ++i) {
      double x = f[i][0] / 2147483648.0;
      CHECK(f[i][0] == f[i][1]);
      mx = std::max(mx, std::fabs(x));
      if (k) st = std::max(st, std::fabs(x - prev));
      prev = x;
    }
  }
  const double want = 0.15 * masterfx_pi::kStemOnPost;
  CHECK(std::fabs(mx - want) < 0.005);
  CHECK(st < want * 2 * M_PI * 220.0 / 48000.0 * 1.05);
  printf("test_masterfx_pi: SRC MIX: the stems' sum on 0/1 at +10.9 dB, L = R, no chain needed\n");
  master::set_src(master::kRet);
  mx = 0;
  for (int k = 0; k < 60; ++k) block(true);
  CHECK(!masterfx_pi::WantStems());
  for (int k = 0; k < 10; ++k) {
    block(false);
    for (unsigned i = 0; i < kFill; ++i) mx = std::max(mx, std::fabs((double)f[i][0]));
  }
  CHECK(mx == 0);
}

// ---- the limiter by itself (core/fx/limiter.h)
static double g_rnd_state = 1;
static double rnd() {   // -1..1
  g_rnd_state = std::fmod(g_rnd_state * 16807.0, 2147483647.0);
  return g_rnd_state / 1073741823.5 - 1.0;
}

static void limiter_alone() {
  static fx::Limiter z;
  // nothing over the ceiling: bursts, sines, noise, up to 36 dB over, left, right and both
  for (double over : {1.5, 8.05, 64.0}) {
    z.Reset();
    double top = 0;
    for (int i = 0; i < 2000000; ++i) {
      const int seg = (i >> 11) & 7;
      double a = seg == 0 ? 0.3 : seg == 1 ? over * std::sin(i * 0.31) : seg == 2 ? over * rnd() : seg == 3 ? (i % 97 == 0 ? over : 0.0) : over * (i & 1 ? 1 : -1) * ((i >> 6) & 1);
      double b = seg == 4 ? 0.2 * rnd() : seg == 5 ? over : a * 0.5;
      float l = (float)a, r = (float)b;
      z.Frame(l, r);
      top = std::max(top, (double)std::max(std::fabs(l), std::fabs(r)));
    }
    CHECK(top <= fx::Limiter::kCeiling);
    printf("test_masterfx_pi: limiter, %.1f dB over: largest sample out %.6f, ceiling %.2f\n", 20 * std::log10(over), top, fx::Limiter::kCeiling);
  }
  // the release's last step: peaks just under the ceiling while the gain comes back past 0.9998
  z.Reset();
  double near = 0;
  for (int i = 0; i < 400000; ++i) {
    float l = i % 60000 == 0 ? 4.0f : (i % 7 == 0 ? 0.98f : 0.0f), r = 0.0f;
    z.Frame(l, r);
    near = std::max(near, (double)std::fabs(l));
  }
  CHECK(near <= fx::Limiter::kCeiling);
  // a NaN on R does not hide a peak on L
  z.Reset();
  double seen = 0;
  for (int i = 0; i < 200; ++i) {
    float l = i == 60 ? 8.0f : 0.0f, r = i == 60 ? std::nanf("") : 0.0f;
    z.Frame(l, r);
    seen = std::max(seen, (double)std::fabs(l));
  }
  CHECK(seen <= fx::Limiter::kCeiling);
  // under the ceiling: the signal, kLook later, and not a bit else
  z.Reset();
  std::vector<float> in;
  for (int i = 0; i < 4000; ++i) {
    in.push_back((float)(0.97 * std::sin(i * 0.2)));
    float l = in.back(), r = -l;
    z.Frame(l, r);
    if (i >= kLook) CHECK(l == in[i - kLook] && r == -in[i - kLook]);
  }
  // the stereo link: a peak on the left only takes the right down by the same gain, and it gets there before the peak
  z.Reset();
  for (int i = 0; i < 2000; ++i) {
    float l = i == 1000 ? 4.0f : 0.0f, r = 0.5f;
    const float g = z.Gain();
    z.Frame(l, r);
    if (i == 1000 + kLook) CHECK(std::fabs(l - 4.0f * z.Gain()) < 1e-5f && std::fabs(r - 0.5f * z.Gain()) < 1e-5f && z.Gain() < 0.246f);
    if (i == 1000 + kLook) CHECK(l <= fx::Limiter::kCeiling && std::fabs(r / l - 0.125f) < 1e-5f);
    if (i < 1000) CHECK(g == 1.0f);
  }
  // recovery: 18 dB of reduction, then quiet: within 1 % after 5 x the release, back at 1 after it
  z.Reset();
  for (int i = 0; i < 4800; ++i) {
    float l = 8.05f * (float)std::sin(i * 0.05), r = l;
    z.Frame(l, r);
  }
  double dip = z.Gain();
  CHECK(dip < 0.125);
  CHECK(z.TakeLow() < 0.125f && z.TakeLow() == 1.0f);
  int back = -1;
  for (int i = 0; i < 48000 && back < 0; ++i) {
    float l = 0.1f, r = 0.1f;
    z.Frame(l, r);
    if (z.Gain() > 0.99f) back = i;
  }
  printf("test_masterfx_pi: limiter: %.1f dB down, within 1 %% after %.0f ms\n", -20 * std::log10(dip), back / 48.0);
  CHECK(back > 19200 && back < 28800);   // 100 ms a time constant, about 4.4 of them
  for (int i = 0; i < 48000; ++i) {
    float l = 0.1f, r = 0.1f;
    z.Frame(l, r);
  }
  CHECK(z.Gain() == 1.0f);
  // the cost, the worst of it: every frame over the ceiling, 12 frames a block
  z.Reset();
  static float buf[1 << 16][2];
  for (auto& b : buf) b[0] = (float)(3.0 * rnd()), b[1] = (float)(3.0 * rnd());
  auto t0 = std::chrono::steady_clock::now();
  float sum = 0;
  for (int k = 0; k < 400; ++k)
    for (auto& b : buf) {
      float l = b[0], r = b[1];
      z.Frame(l, r);
      sum += l;
    }
  double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (400.0 * (1 << 16));
  printf("test_masterfx_pi: limiter: %.1f ns a frame on this Mac under noise 10 dB over (%s)\n", ns, sum == 1e9f ? "!" : "ok");
}

// SRC MIX, a kit 18 dB over: nothing over the ceiling on 0/1, a level loss of the sine's shape only (no tanh first), the same
// gain on both sides, the reading; 2/3 and the tracks not touched; the trim moves it; the way out ends bit for bit
static double g_stem = 0.0;
static void stems_block(int32_t (*f)[12], Gen g = nullptr) {
  static int32_t s[64][8];
  for (unsigned i = 0; i < kFill; ++i) {
    const int32_t x = (int32_t)(g_stem / 8.0 * 2147483648.0 * std::sin(2 * M_PI * 110.0 * (double)(g_t + i) / 48000.0));
    for (int v = 0; v < 8; ++v) s[i][v] = x;
  }
  masterfx_pi::Stems(s, kFill);
  memset(f, 0, sizeof(int32_t) * 12 * kFill);
  if (g)
    for (unsigned i = 0; i < kFill; ++i) g(g_t + i, f[i]);
  masterfx_pi::Process(f, kFill);
  masterfx_pi::Render();
  g_t += kFill;
}

static void through_limited() {
  off();
  master::set_src(master::kMix);
  master::set_trim(0);
  const double trims = (1 + 1 + 1 + 1 + 1 + 2 * masterfx_pi::kStemHotTrim + 1) / 8;   // MT/HT and CH/OH are trimmed: their share
  g_stem = 8.05 / masterfx_pi::kStemOnPost / trims;   // 18 dB over on 0/1, as the owner's kit
  int32_t f[64][12];
  for (int k = 0; k < 300; ++k) stems_block(f);
  double top = 0, e = 0;
  long n = 0;
  for (int k = 0; k < 3000; ++k) {
    stems_block(f, all_sine);
    for (unsigned i = 0; i < kFill; ++i) {
      double a = f[i][0] / 2147483648.0, b = f[i][1] / 2147483648.0;
      top = std::max(top, std::max(std::fabs(a), std::fabs(b)));
      e += a * a, ++n;
      CHECK(f[i][0] == f[i][1] || std::fabs(a - b) < 0.6);   // (0/1 of all_sine differ; both under one gain)
      int32_t want[12];
      memset(want, 0, sizeof want);
      all_sine(g_t - kFill + i, want);
      CHECK(!memcmp(f[i] + 2, want + 2, sizeof(int32_t) * 10));
    }
  }
  CHECK(top <= fx::Limiter::kCeiling * (1 + 1e-6) && top > 0.97);
  // a sine held under the ceiling by one gain stays a sine: RMS is its peak / sqrt 2. The soft clip made it squarer (0.8 and up)
  const double rms = std::sqrt(e / (double)n);
  printf("test_masterfx_pi: SRC MIX 18 dB over, trim 0: largest sample %.6f, RMS %.4f, reduction %.1f dB\n", top, rms, master::gr10() / 10.0);
  CHECK(rms < 0.74);
  CHECK(master::gr10() > 165 && master::gr10() < 205);
  char o[900];
  CHECK(master::json(o, sizeof o) > 0 && strstr(o, "\"trim\":0,\"gr\":1"));
  g_log.clear();
  CHECK(masterfx_pi::Command("mfx=stats") && g_log.find("limiter 1") != std::string::npos && g_log.find(" dB") != std::string::npos);
  // the trim, smoothed: 12 dB less into it, no step on the way
  master::set_trim(-12);
  double step = 0, prev = f[kFill - 1][0] / 2147483648.0;
  for (int k = 0; k < 40; ++k) {
    stems_block(f);
    for (unsigned i = 0; i < kFill; ++i) {
      double a = f[i][0] / 2147483648.0;
      step = std::max(step, std::fabs(a - prev)), prev = a;
    }
  }
  CHECK(step < 0.06);
  for (int k = 0; k < 2500; ++k) stems_block(f);   // the release, and the reading's second
  for (int k = 0; k < 2500; ++k) stems_block(f);
  printf("test_masterfx_pi: trim -12: reduction %.1f dB, largest step on the way %.4f\n", master::gr10() / 10.0, step);
  CHECK(master::gr10() > 45 && master::gr10() < 85);
  // out again: the stems first, then the limiter, then not a sample touched
  master::set_src(master::kRet);
  g_stem = 0;
  for (int k = 0; k < 60; ++k) stems_block(f);
  CHECK(master::gr10() == 0 && !masterfx_pi::WantStems());
  for (int k = 0; k < 20; ++k) {
    long t = g_t;
    fill(all_sine, f, kFill);
    for (unsigned i = 0; i < kFill; ++i) {
      int32_t want[12];
      memset(want, 0, sizeof want);
      all_sine(t + i, want);
      CHECK(!memcmp(f[i], want, sizeof want));
    }
  }
  masterfx_pi::Reset();
}

// no click: the limiter and the stems in and out under a return that plays on, and under hot stems
static void limiter_fades() {
  off();
  int32_t f[64][12];
  for (double level : {0.0, 0.1, 1.0}) {
    master::set_trim(0);
    double step = 0, prev = 0, top = 0;
    bool have = false;
    for (int k = 0; k < 8000; ++k) {
      if (k % 228 == 0) master::set_src(k % 456 ? master::kRet : master::kMix);
      g_stem = level / masterfx_pi::kStemOnPost;
      stems_block(f, post_sine);
      for (unsigned i = 0; i < kFill; ++i) {
        double a = f[i][0] / 2147483648.0;
        if (have) step = std::max(step, std::fabs(a - prev));
        top = std::max(top, std::fabs(a));
        prev = a, have = true;
      }
    }
    const double own = 0.2 * 2 * M_PI * 220.0 / 48000.0 + level * 2 * M_PI * 110.0 / 48000.0;
    printf("test_masterfx_pi: SRC switched every 100 ms for 17 s, stems %.1f: largest step %.5f, the signals' own %.5f, largest sample %.4f\n", level, step, own, top);
    CHECK(step < own * 1.4 + 2.0 * 1.2 / masterfx_pi::kFadeFrames);
    CHECK(top <= 1.2 + 1e-6);
    master::set_src(master::kRet);
    g_stem = 0;
    for (int k = 0; k < 60; ++k) stems_block(f);
    masterfx_pi::Reset();
  }
}

// SRC RET: the chain gets the limiter too, in once it has something to hold: hot input and LEV, nothing over the ceiling, and
// bypassed it leaves with the chain
static void chain_limited() {
  off();
  flat_chain();
  master::set_lev(127);
  int32_t f[64][12];
  static auto hot = [](long t, int32_t* ch) {
    ch[0] = (int32_t)(0.95 * 2147483648.0 * std::sin(2 * M_PI * 220.0 * (double)t / 48000.0));
    ch[1] = ch[0] / 2;
    ch[2] = ch[3] = ch[0] / 2;
  };
  for (int k = 0; k < 100; ++k) fill(hot, f, kFill);
  CHECK(masterfx_pi::Wet() == 1.0f);
  double top = 0;
  for (int k = 0; k < 2000; ++k) {
    fill(hot, f, kFill);
    for (unsigned i = 0; i < kFill; ++i) top = std::max(top, std::fabs(f[i][0] / 2147483648.0));
  }
  printf("test_masterfx_pi: SRC RET, a chain asking for +9 dB: largest sample %.6f, reduction %.1f dB\n", top, master::gr10() / 10.0);
  CHECK(top <= fx::Limiter::kCeiling * (1 + 1e-6) && top > 0.97 && master::gr10() > 50);
}

// stereo=measure in SRC MIX: stems and limiter out, the plain return, bit for bit; the stems' queue does not back up
static void measure_through_mode() {
  off();
  master::set_src(master::kMix);
  g_stem = 0.2;
  int32_t f[64][12];
  for (int k = 0; k < 100; ++k) stems_block(f);
  char o[120];
  CHECK(out::command("stereo=measure", o, sizeof o));
  for (int k = 0; k < 60; ++k) stems_block(f);
  for (int k = 0; k < 100; ++k) {
    long t = g_t;
    stems_block(f, all_sine);
    for (unsigned i = 0; i < kFill; ++i) {
      int32_t want[12];
      memset(want, 0, sizeof want);
      all_sine(t + i, want);
      CHECK(!memcmp(f[i], want, sizeof want));
    }
  }
  out::meas::reset();
  out::reset();
  uint32_t u0 = masterfx_pi::Read().stem_under;
  for (int k = 0; k < 100; ++k) stems_block(f);
  CHECK(masterfx_pi::Read().stem_under == u0);
  double top = 0;
  for (int k = 0; k < 100; ++k) {
    stems_block(f);
    for (unsigned i = 0; i < kFill; ++i) top = std::max(top, std::fabs(f[i][0] / 2147483648.0));
  }
  CHECK(top > 0.1);
  printf("test_masterfx_pi: stereo=measure in SRC MIX: the plain return bit for bit, the stems back after it\n");
  master::set_src(master::kRet);
  g_stem = 0;
  for (int k = 0; k < 60; ++k) stems_block(f);
  masterfx_pi::Reset();
}

int main() {
  limiter_alone();
  nothing_set_touches_nothing();
  bypass_bit_exact();
  latency();
  returns_go_through();
  no_click();
  measure_stands_aside();
  dry_ring();
  src_mix();
  src_mix_report();
  through_limited();
  limiter_fades();
  chain_limited();
  measure_through_mode();
  CHECK(masterfx_pi::Command("mfx=stats") && g_log.find("core 2 load") != std::string::npos);
  CHECK(!masterfx_pi::Command("mfx"));
  printf("test_masterfx_pi: PASS\n");
  return 0;
}
