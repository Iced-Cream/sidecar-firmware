// core/tests/test_ufx_ambient.cpp - the ambient user effects NIMBUS (plugins/ufx_nimbus) and GALACTIC
// (plugins/ufx_galactic) as the send FX plays them: real RMPA packages run in place on this arm64 Mac (kept built in
// tests/api3_pkgs: `make -C plugins/ufx_nimbus fixtures`, `make -C plugins/ufx_galactic fixtures`; CI has no AArch64
// compiler), loaded into UFX1 / UFX2 by um and run through fx::SendFx as core 2 runs them. Checked:
//   - both load (API 3, an effect, the delay memory in the arena) and give the Rytm their knob names;
//   - NIMBUS: DCY sets the tail's length, TONE closes the top, the sides differ, ENS adds the pad (a held sine comes
//     back with the ensemble chorus's 5.9 Hz sidebands, which the reverb alone does not make), the pad sounds and then
//     ends with a short decay;
//   - GALACTIC: REPL sets the tail's length and holds it at 0, BRT closes the top, the sides differ, BIG turned down
//     and up in a tail keeps playing;
//   - the loudest settings of both: the slot is not muted and the tail is alive and falling (the host turns a NaN
//     into silence and the send's limiter holds a runaway, so "finite and bounded" alone would prove nothing here);
//   - the cost of one send slot's call on this Mac (printed; the Pi's estimate: core/tests/bench/ufx_bench.sh).
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "engine.h"
#include "fx/send_fx.h"
#include "hooks.h"
#include "lfo.h"
#include "rmp_slots.h"
#include "um_host.h"
#include "user/um.h"

using namespace core;

#if defined(__aarch64__)
static std::vector<std::string> g_log;
static void sink(const char* s, int n) { g_log.emplace_back(s, (size_t)n); }
static void out(const uint8_t*, int) {}
static std::string slurp(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  CHECK(f);
  std::string b;
  char t[65536];
  size_t n;
  while ((n = fread(t, 1, sizeof t, f)) > 0) b.append(t, n);
  fclose(f);
  return b;
}
static void put(const std::string& p, const std::string& b) {
  FILE* f = fopen(host_path(p.c_str()).c_str(), "wb");
  CHECK(f);
  fwrite(b.data(), 1, b.size(), f);
  fclose(f);
}
static void sysex(std::vector<uint8_t> m) { engine_sysex(m.data(), (int)m.size()); }
static void idle_all() {
  int quiet = 0;
  for (int i = 0; i < 200000; ++i) {
    if (um::idle()) quiet = 0;
    else if (++quiet > 8) return;
  }
  CHECK(!"um::idle never settled");
}
static std::string cmd(const std::string& c) {
  char o[200];
  CHECK(um::command(c.c_str(), o, sizeof o));
  idle_all();
  return o;
}
static int logged(const std::string& s) {
  int n = 0;
  for (auto& l : g_log) n += l.find(s) != std::string::npos;
  return n;
}
static uint64_t g_t = 1000000;

// ---- the send FX with one effect on FX 1 (TYPE 64 + u), BD's stem sent fully; x the input, L/R the return
struct Run {
  std::unique_ptr<fx::SendFx> sfx;
  int u;
  Run(int u_, int p1, int p2, int p3, int p4) : sfx(std::make_unique<fx::SendFx>(48000.0f)), u(u_) {
    cmd("um=reload:f" + std::to_string(u + 1));   // a fresh state (the effect's states are um's, not the SendFx's)
    memset((void*)&fx::control, 0, sizeof fx::control);
    fx::control.send[0][0] = 127;
    fx::control.knob[0][0] = (uint8_t)(fx::kFirstUser + u);
    fx::control.knob[0][1] = 0;     // ROUTE PRE (its limiter at -6 dBFS: the levels below stay under it)
    fx::control.knob[0][2] = (uint8_t)p1, fx::control.knob[0][3] = (uint8_t)p2;
    fx::control.knob[0][4] = (uint8_t)p3, fx::control.knob[0][5] = (uint8_t)p4;
    CHECK(fx::state(0) == fx::kStReady);
  }
  void go(const std::vector<float>& x, std::vector<float>* L, std::vector<float>* R) {
    size_t n = x.size() / 12 * 12;
    L->assign(n, 0.0f), R->assign(n, 0.0f);
    float st[12][fx::kStems], o[12][2], m[12][2];
    for (size_t i = 0; i < n; i += 12) {
      for (int j = 0; j < 12; ++j) {
        for (int k = 0; k < fx::kStems; ++k) st[j][k] = 0.0f;
        st[j][0] = x[i + j];
      }
      um::pre_block();   // the block's transport snapshot (the platform's audio loop does this)
      um::idle();        // the control core: the effect's fresh state when its TYPE came
      sfx->Process(st, 12, o, fx::control, m);
      for (int j = 0; j < 12; ++j) (*L)[i + j] = m[j][0] + o[j][0], (*R)[i + j] = m[j][1] + o[j][1];
    }
  }
};
static bool sane(const std::vector<float>& y, float limit) {   // finite, no denormals, under limit
  for (float v : y)
    if (!std::isfinite(v) || std::fabs(v) > limit || (v != 0.0f && std::fpclassify(v) == FP_SUBNORMAL)) return false;
  return true;
}
static float rms(const std::vector<float>& y, size_t a, size_t b) {
  double s = 0.0;
  for (size_t i = a; i < b && i < y.size(); ++i) s += (double)y[i] * y[i];
  return b > a ? (float)std::sqrt(s / (double)(b - a)) : 0.0f;
}
static std::vector<float> sine(float hz, size_t n, float amp) {
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i) x[i] = amp * (float)std::sin(2.0 * M_PI * hz * (double)i / 48000.0);
  return x;
}
static std::vector<float> noise(size_t n, float amp, uint32_t seed = 1) {
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i) {
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    x[i] = amp * ((float)(int32_t)seed / 2147483648.0f);
  }
  return x;
}
// a burst of noise (seconds), then silence up to total seconds
static std::vector<float> burst(float seconds, float total, float amp) {
  std::vector<float> x = noise((size_t)(seconds * 48000.0f), amp, 5);
  x.resize((size_t)(total * 48000.0f), 0.0f);
  return x;
}
// the share of y's energy in [a, b) above about 4 kHz (a first difference against the plain sum: a cheap tilt meter)
static float hf_share(const std::vector<float>& y, size_t a, size_t b) {
  double e = 0.0, d = 0.0;
  for (size_t i = a + 1; i < b && i < y.size(); ++i) e += (double)y[i] * y[i], d += (double)(y[i] - y[i - 1]) * (y[i] - y[i - 1]);
  return e > 0.0 ? (float)(d / e) : 0.0f;
}
static float corr(const std::vector<float>& L, const std::vector<float>& R, size_t a) {
  double c = 0.0, el = 0.0, er = 0.0;
  for (size_t i = a; i < L.size(); ++i) c += (double)L[i] * R[i], el += (double)L[i] * L[i], er += (double)R[i] * R[i];
  return el > 0.0 && er > 0.0 ? (float)(c / std::sqrt(el * er)) : 1.0f;
}
// the level of y at hz over [a, b), a Hann window (the bin's magnitude, comparable between frequencies)
static float level_at(const std::vector<float>& y, size_t a, size_t b, float hz) {
  double re = 0.0, im = 0.0;
  for (size_t i = a; i < b; ++i) {
    double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * (double)(i - a) / (double)(b - a)), ph = 2.0 * M_PI * hz * (double)i / 48000.0;
    re += w * y[i] * std::cos(ph), im += w * y[i] * std::sin(ph);
  }
  return (float)std::sqrt(re * re + im * im);
}

int main() {
  char tmpl[] = "/tmp/core-ufxa-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  host_set_log_sink(sink);
  host_fake_time(true, g_t);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples") && fs.mkdir("/Plugins") && fs.mkdir("/.rytm/projects/UF"));
  put("/.rytm/project", "UF");
  for (const char* n : {"ufx_nimbus", "ufx_galactic"})
    put(std::string("/Plugins/") + n + ".rmp", slurp(std::string("tests/api3_pkgs/") + n + ".a64.rmp"));
  hooks.to_rytm = ::out;
  CHECK(engine_begin());
  um::begin(host_um_exec());
  sysex({0xf0, 0x7d, 0x52, 0x54, 0x10, 'U', 'F', 0xf7});
  idle_all();
  cmd("um=empty");

  // ---- both load, and their labels
  cmd("um=fx:1:fx.nimbus");
  cmd("um=fx:2:fx.galactic");
  um::SlotView v = um::slot_view(5);
  CHECK(v.state == RMP_SLOT_READY && v.api == 3 && !v.muted);
  CHECK(v.arena >= 2u * 152576u * sizeof(float));   // 2 states (FX 1, FX 2) x NIMBUS's lines
  v = um::slot_view(6);
  CHECK(v.state == RMP_SLOT_READY && v.api == 3 && !v.muted);
  CHECK(v.arena >= 2u * 2u * 66031u * sizeof(float));   // 2 states x 2 sides of GALACTIC's delays
  CHECK(logged("um: UFX1 fx.nimbus: ready (API 3, arena ") == 1);
  CHECK(logged("um: UFX2 fx.galactic: ready (API 3, arena ") == 1);
  CHECK(std::string(fx::user[0].name) == "NIMBUS" && std::string(fx::user[1].name) == "GALACTIC");
  {
    const fx::KnobLabels* l = fx::labels(fx::kFirstUser);
    CHECK(l && std::string(l->label[0]) == "DCY" && std::string(l->label[1]) == "ENS");
    CHECK(l && std::string(l->label[2]) == "TONE" && std::string(l->label[3]) == "MOD");
    l = fx::labels(fx::kFirstUser + 1);
    CHECK(l && std::string(l->label[0]) == "REPL" && std::string(l->label[1]) == "BRT");
    CHECK(l && std::string(l->label[2]) == "DTUN" && std::string(l->label[3]) == "BIG");
  }
  std::vector<float> L, R, L2, R2;

  // ---- NIMBUS
  {   // DCY: a burst's tail 3..4 s after it, short (0.5 s) against long (~20 s); the sides differ
    std::vector<float> x = burst(0.5f, 4.5f, 0.1f);
    Run a(0, 0, 0, 80, 64);
    a.go(x, &L, &R);
    Run b(0, 96, 0, 80, 64);
    b.go(x, &L2, &R2);
    float ts = rms(L, 3.5f * 48000, 4.5f * 48000), tl = rms(L2, 3.5f * 48000, 4.5f * 48000), c = corr(L2, R2, 48000);
    printf("test_ufx_ambient: NIMBUS DCY: tail 3 s after a burst %.2e (DCY 0), %.2e (DCY 96); L/R correlation %.3f\n", ts, tl, c);
    CHECK(sane(L, 2.0f) && sane(R, 2.0f) && sane(L2, 2.0f) && sane(R2, 2.0f));
    CHECK(ts < 1e-6f && tl > 1e-3f);
    CHECK(c < 0.5f && c > -0.5f);
  }
  {   // TONE: the tail's tilt, closed against open
    std::vector<float> x = burst(1.0f, 3.0f, 0.1f);
    Run a(0, 80, 0, 0, 64);
    a.go(x, &L, &R);
    Run b(0, 80, 0, 127, 64);
    b.go(x, &L2, &R2);
    float dark = hf_share(L, 72000, L.size()), open = hf_share(L2, 72000, L2.size());
    printf("test_ufx_ambient: NIMBUS TONE: tilt %.5f closed, %.5f open\n", dark, open);
    CHECK(dark < 0.1f * open);
  }
  {   // ENS: a held 440 Hz sine, the lines' modulation off: only the ensemble's chorus makes sidebands 5.9 Hz away
    std::vector<float> x = sine(440.0f, 48000 * 5, 0.05f);
    Run a(0, 60, 0, 80, 0);
    a.go(x, &L, &R);
    Run b(0, 60, 127, 80, 0);
    b.go(x, &L2, &R2);
    size_t s0 = 48000 * 3, s1 = 48000 * 5;
    float off = level_at(L, s0, s1, 445.9f) / level_at(L, s0, s1, 440.0f);
    float on = level_at(L2, s0, s1, 445.9f) / level_at(L2, s0, s1, 440.0f);
    printf("test_ufx_ambient: NIMBUS ENS: the 5.9 Hz sideband against the sine %.4f (ENS 0), %.4f (ENS 127)\n", off, on);
    CHECK(off < 0.02f && on > 0.1f);
  }
  {   // the pad ends with a short decay: a sine, then silence, DCY 0 (the pad's release 0.2 s)
    std::vector<float> x = sine(440.0f, 48000 * 2, 0.05f);
    x.resize(48000 * 8, 0.0f);
    Run a(0, 0, 127, 80, 64);
    a.go(x, &L, &R);
    Run b(0, 0, 0, 80, 64);   // the same without the pad: the reverb's own tail there
    b.go(x, &L2, &R2);
    size_t p0 = 48000 * 2 + 14400, p1 = p0 + 9600;   // 0.3 .. 0.5 s after the sine ends
    float at = rms(L, p0, p1), rev = rms(L2, p0, p1), end = rms(L, 48000 * 7, 48000 * 8);
    printf("test_ufx_ambient: NIMBUS pad: 0.3 s after the sine ends %.2e with ENS 127, %.2e with ENS 0; %.2e 5 s after it (DCY 0)\n",
           at, rev, end);
    CHECK(sane(L, 2.0f) && sane(R, 2.0f) && at > 3.0f * rev && end < 1e-6f);
  }
  {   // the loudest settings: full-scale noise for 4 s, then 6 s of silence
    std::vector<float> x = burst(4.0f, 10.0f, 1.0f);
    Run a(0, 127, 127, 127, 127);
    a.go(x, &L, &R);
    CHECK(sane(L, 4.0f) && sane(R, 4.0f));
    // (the send's limiter holds a runaway near 0.5, so a peak proves nothing: the tail must be alive and falling)
    float early = rms(L, 48000 * 9 / 2, 48000 * 11 / 2), tail = rms(L, L.size() - 48000, L.size());
    printf("test_ufx_ambient: %s loudest: the tail %.3f just after the noise, %.3f 5 s later\n", "NIMBUS", early, tail);
    CHECK(!um::slot_view(5).muted && tail > 1e-3f && tail < 0.95f * early);
  }

  // ---- GALACTIC
  {   // REPL: the tail 3..4 s after a burst, replaced fast (127) against slowly (30); the sides differ
    std::vector<float> x = burst(0.5f, 4.5f, 0.1f);
    Run a(1, 127, 64, 64, 127);
    a.go(x, &L, &R);
    Run b(1, 30, 64, 64, 127);
    b.go(x, &L2, &R2);
    float ts = rms(L, 3.5f * 48000, 4.5f * 48000), tl = rms(L2, 3.5f * 48000, 4.5f * 48000), c = corr(L2, R2, 48000);
    printf("test_ufx_ambient: GALACTIC REPL: tail 3 s after a burst %.2e (REPL 127), %.2e (REPL 30); L/R correlation %.3f\n", ts, tl, c);
    CHECK(sane(L, 2.0f) && sane(R, 2.0f) && sane(L2, 2.0f) && sane(R2, 2.0f));
    CHECK(tl > 10.0f * ts && tl > 1e-3f);
    CHECK(c < 0.9f);
  }
  {   // BRT: the tail's tilt, dark against bright
    std::vector<float> x = burst(1.0f, 3.0f, 0.1f);
    Run a(1, 38, 20, 64, 127);
    a.go(x, &L, &R);
    Run b(1, 38, 127, 64, 127);
    b.go(x, &L2, &R2);
    float dark = hf_share(L, 72000, L.size()), open = hf_share(L2, 72000, L2.size());
    printf("test_ufx_ambient: GALACTIC BRT: tilt %.5f dark, %.5f bright\n", dark, open);
    CHECK(dark < 0.1f * open);
  }
  {   // the loudest settings: the longest tail, full-scale noise for 4 s, then 6 s of silence
    std::vector<float> x = burst(4.0f, 10.0f, 1.0f);
    Run a(1, 1, 127, 127, 127);
    a.go(x, &L, &R);
    CHECK(sane(L, 4.0f) && sane(R, 4.0f));
    // (the send's limiter holds a runaway near 0.5, so a peak proves nothing: the tail must be alive and falling)
    float early = rms(L, 48000 * 9 / 2, 48000 * 11 / 2), tail = rms(L, L.size() - 48000, L.size());
    printf("test_ufx_ambient: %s loudest: the tail %.3f just after the noise, %.3f 5 s later\n", "GALACTIC", early, tail);
    CHECK(!um::slot_view(6).muted && tail > 1e-3f && tail < 0.95f * early);
  }
  {   // REPL 0 holds what is in the tank: the level 1 s and 5 s after a burst is the same
    std::vector<float> x = burst(1.0f, 2.0f, 0.1f);
    Run a(1, 40, 64, 64, 127);
    a.go(x, &L, &R);            // fill it, then freeze
    fx::control.knob[0][2] = 0;
    x.assign(48000 * 6, 0.0f);
    a.go(x, &L, &R);
    float e1 = rms(L, 48000, 48000 * 2), e5 = rms(L, 48000 * 5, 48000 * 6);
    printf("test_ufx_ambient: GALACTIC REPL 0: %.2e 1 s into the freeze, %.2e at 5 s\n", e1, e5);
    CHECK(e1 > 1e-3f && e5 > 0.7f * e1 && e5 < 1.3f * e1);
  }
  {   // BIG turned down and up again in a tail (the delays' lengths change under their counters)
    std::vector<float> x = burst(1.0f, 2.0f, 0.1f), z(4800, 0.0f);
    Run a(1, 20, 64, 64, 127);
    a.go(x, &L, &R);
    float pk = 0.0f;
    for (int big : {90, 40, 0, 30, 127, 64, 127}) {
      fx::control.knob[0][5] = (uint8_t)big;
      a.go(z, &L, &R);
      CHECK(sane(L, 2.0f) && sane(R, 2.0f));
      for (float y : L) pk = std::fabs(y) > pk ? std::fabs(y) : pk;
    }
    z.assign(48000, 0.0f);
    a.go(z, &L, &R);
    printf("test_ufx_ambient: GALACTIC BIG moved in a tail: peak %.3f, the tail after it %.2e\n", pk, rms(L, 0, L.size()));
    CHECK(!um::slot_view(6).muted && pk < 0.99f && rms(L, 0, L.size()) > 1e-4f);
  }

  // ---- the cost of one call (one send slot, 12 frames) on this Mac, busy settings
  for (int u = 0; u < 2; ++u) {
    Run r(u, 96, 127, 100, 127);
    std::vector<float> x = noise(12 * 4000, 0.5f, 13);
    r.go(x, &L, &R);   // warm
    auto t0 = std::chrono::steady_clock::now();
    r.go(x, &L, &R);
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 4000.0;
    printf("test_ufx_ambient: %s: %.2f us a 12-frame block of the whole send FX on this Mac (the effect and the host's mix)\n",
           u ? "GALACTIC" : "NIMBUS", us);
  }
  cmd("um=fx:1:-");
  cmd("um=fx:2:-");
  CHECK(!fx::labels(fx::kFirstUser) && !fx::labels(fx::kFirstUser + 1));
  printf("test_ufx_ambient: PASS\n");
  return 0;
}
#else
int main() {
  printf("test_ufx_ambient: SKIP (the RMPA packages run in place on an arm64 host only)\n");
  return 0;
}
#endif
