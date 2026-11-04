// plugins/modele/tests/md_check.cpp - the host checks of MODEL E: the RMPA package (make package: out/modele.a64.rmp)
// loaded and run on this arm64 Mac as the Pi runs it (rmp_loader: verify, the two parts placed, relocated; MAP_JIT):
//   load      verifies, places, init()s; the header's flags (instancing, pitch note); the state fits the limits
//   level     a bass line and notes across the keyboard at the defaults: rms in range, no clipping, silent after
//   pitch     each oscillator, each range: within 2 cents (Analog off), 9 with the drift; the tuning laws; CONTROL off
//             frees osc 3 of the keys and widens its tuning; the free LO table
//   filter    the ladder's response against an open filter: -12 dB at the cutoff, 24 dB an octave, keyboard tracking,
//             the contour opening and closing it, emphasis raising the cutoff's gain
//   selfosc   no input, EMPH up: oscillates on the cutoff (within 15 cents) across the range, tracks the keys, EMPH
//             below the threshold rings out, the level stays bounded
//   glide     pitch moves in small monotone steps to the target, without a jump; the time; no glide jumps at once
//   click     note on, note off, retrigger, a stolen voice: no sample step over the limit (a slow wave, so any step
//             there is the voice's), sweeps of CUT over a held note make no step either
//   mod       osc 3 at LO: vibrato on osc 1 (span against depth), audio-rate modulation stays finite
//   extreme   every knob at each end alone, all at 0, all at 127, all random every block, EMPH max with fast modulation
//             across the keyboard: finite, within +-1, nothing written outside the state
//   denormal  silent input and long tails: no subnormal output, the time of a silent ladder like a playing one's
//   chunks    render(n) for n 1..12 sounds like render(12)
//   cpu       ns per 12-frame call: the patches, against HUSH1's, the Pi estimate
// GPL-2.0-or-later, Copyright 2026 trigcondition and contributors.
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rmp_api.h"
#include "rmp_loader.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); ++g_fail; } } while (0)

static const int kFs = 48000, kBlock = 12;
static double now_ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC_RAW, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}
static void hlog(const char *m) { printf("  plugin log: %s\n", m); }
static uint32_t hrand(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static rmp_host g_host = {RMP_API, (float)kFs, kBlock, hlog, hrand, {}};
static rmp_host g_host1 = {1u, (float)kFs, kBlock, hlog, hrand, {}};   // sine_test is an API-1 plugin

static const rmp_plugin *load(const char *path, rmp_info &info) {
  FILE *f = fopen(path, "rb");
  if (!f) return nullptr;
  static std::vector<uint8_t> bytes;
  uint8_t b[65536];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) bytes.insert(bytes.end(), b, b + n);
  fclose(f);
  rmp_host_target = RMP_TARGET_A64;
  rmp_status vs = rmp_verify(bytes.data(), (uint32_t)bytes.size(), &info);
  if (vs != RMP_OK) { printf("  verify: %d\n", (int)vs); return nullptr; }
  uint32_t code = (info.data_start + 4095u) & ~4095u;
  uint32_t data = (info.image_size - info.data_start + info.bss_size + 4095u) & ~4095u;
  if (!data) data = 4096;
  uint8_t *c = (uint8_t *)mmap(nullptr, code, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (c == MAP_FAILED) return nullptr;
  uint8_t *d = (uint8_t *)mmap(c + code, data, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (d == MAP_FAILED) return nullptr;
  pthread_jit_write_protect_np(0);
  rmp_status st = rmp_place_a64(&info, c, (uintptr_t)c, d, (uintptr_t)d);
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(c, code);
  if (st != RMP_OK) { printf("  place: %d\n", (int)st); return nullptr; }
  const rmp_plugin *p = (const rmp_plugin *)(c + info.entry_offset);
  return p->magic == RMP_PLUGIN_MAGIC && p->init(&g_host) == 0 ? p : nullptr;
}

struct Voice {
  const rmp_plugin *p;
  std::vector<uint8_t> mem;
  uint8_t *st;
  enum { kGuard = 256 };
  explicit Voice(const rmp_plugin *pl) : p(pl), mem(pl->state_bytes + 2 * kGuard + 8, 0xA5) {
    st = (uint8_t *)(((uintptr_t)mem.data() + kGuard + 7) & ~(uintptr_t)7);
    memset(st, 0, p->state_bytes);
    p->voice_init(st);
  }
  bool guards_ok() const {
    for (const uint8_t *q = mem.data(); q < st; ++q) if (*q != 0xA5) return false;
    for (const uint8_t *q = st + p->state_bytes; q < mem.data() + mem.size(); ++q) if (*q != 0xA5) return false;
    return true;
  }
  void params(const float *p16) { p->params(st, p16); }
  void on(float n) { p->note_on(st, n, 0.8f); }
  void off() { p->note_off(st); }
  void run(std::vector<float> &x, int from, int to) {
    for (int i = from; i + kBlock <= to; i += kBlock) p->render(st, &x[i], kBlock);
  }
};

// the knobs, P1 = [0]
enum { OSC1, OSC2, OSC3, TUN2, TUN3, MIX, O3NZ, MOD, CUT, EMPH, CONT, GA, DEC, SUS, MODE, LEV };
static const float kDef[16] = {72, 72, 56, 70, 64, 64, 104, 64, 44, 44, 72, 64, 66, 64, 20, 100};   // manifest.json
static float band(int i) { return (float)(8 * i + 4); }     // the centre of a 16-name list's entry
static float osc(int i) { return (float)((i * 128 + 64) / 36); }   // the centre of a 36-name list's entry (OSC1..3)
enum { K0 = 8, K1 = 9, K2 = 10, K3 = 11 };                  // MODE: Analog off, no release; +4 the DECAY switch; -8 Analog on
enum { O1_8TRI = 18, O1_8SAW = 20, O2_8TRI = 18, O3_LOTRI = 0, O3_8TRI = 18, O3_8SAW = 20 };   // 6 x range + wave

// a plain patch: the wave in the filter's open door, full sustain, no contour, Analog off
static void plain(float *p) {
  memcpy(p, kDef, 16 * sizeof(float));
  p[OSC1] = osc(O1_8TRI); p[OSC2] = osc(O2_8TRI); p[OSC3] = osc(O3_8TRI);
  p[TUN2] = 64; p[TUN3] = 64; p[MIX] = 0; p[O3NZ] = 64; p[MOD] = 64;
  p[CUT] = 127; p[EMPH] = 0; p[CONT] = 0; p[GA] = 64; p[DEC] = 20; p[SUS] = 127; p[MODE] = band(K0); p[LEV] = 100;
}
static void osc1_only(float *p) { p[MIX] = 6.0f; }     // osc 1 alone
static void osc2_only(float *p) { p[MIX] = 121.0f; }   // osc 2 alone

static double rms(const std::vector<float> &x, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < b && i < x.size(); ++i) s += (double)x[i] * x[i];
  return b > a ? std::sqrt(s / (double)(b - a)) : 0.0;
}
static float peak(const std::vector<float> &x) {
  float pk = 0;
  for (float s : x) pk = std::max(pk, std::fabs(s));
  return pk;
}
static bool finite_in(const std::vector<float> &x) {
  for (float v : x) if (!std::isfinite(v) || v < -1.0f || v > 1.0f) return false;
  return true;
}
// the frequency from upward zero crossings (interpolated) in x[a, b)
static double freq_of(const std::vector<float> &x, size_t a, size_t b) {
  double first = -1, last = -1;
  int n = 0;
  for (size_t i = a + 1; i < b; ++i)
    if (x[i - 1] < 0 && x[i] >= 0) {
      double t = (double)(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
      if (first < 0) first = t; else ++n;
      last = t;
    }
  return n > 0 ? n * (double)kFs / (last - first) : 0.0;
}
static double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }
static double note_hz(double n) { return 440.0 * std::pow(2.0, (n - 69.0) / 12.0); }
// the amplitude at f in x[a, b) (a Hann-windowed correlation)
static double amp_at(const std::vector<float> &x, size_t a, size_t b, double f) {
  double re = 0, im = 0, wsum = 0, w0 = 2 * M_PI * f / kFs;
  for (size_t i = a; i < b; ++i) {
    double w = 0.5 - 0.5 * std::cos(2 * M_PI * (double)(i - a) / (double)(b - a));
    re += w * x[i] * std::cos(w0 * (double)i);
    im += w * x[i] * std::sin(w0 * (double)i);
    wsum += w;
  }
  return 2.0 * std::hypot(re, im) / wsum;
}
static double db(double a, double b) { return 20.0 * std::log10((a + 1e-30) / (b + 1e-30)); }
static float max_step(const std::vector<float> &x, size_t a, size_t b) {
  float m = 0;
  for (size_t i = a + 1; i < b && i < x.size(); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
  return m;
}
// one note held for `len` samples on a fresh voice
static std::vector<float> hold(const rmp_plugin *p, const float *prm, float note, int len) {
  Voice v(p);
  v.params(prm);
  v.on(note);
  std::vector<float> x(len / kBlock * kBlock);
  v.run(x, 0, (int)x.size());
  return x;
}
// the CUT knob that puts the cutoff at fc (20 Hz x 2^(10 c))
static float cut_for(double fc) { return (float)(127.0 * std::log2(fc / 20.0) / 10.0); }

static double time_voice(const rmp_plugin *p, const float *prm, float note = 60.0f) {
  Voice v(p);
  v.params(prm);
  p->note_on(v.st, note, 0.8f);
  float o[kBlock];
  for (int i = 0; i < 2000; ++i) v.p->render(v.st, o, kBlock);
  const int blocks = 20000;
  double mean = 1e30;
  for (int run = 0; run < 25; ++run) {   // (the Mac may put a run on a slow core)
    double t0 = now_ns();
    for (int i = 0; i < blocks; ++i) v.p->render(v.st, o, kBlock);
    mean = std::min(mean, (now_ns() - t0) / blocks);
  }
  return mean;
}

int main(int argc, char **argv) {
  const char *pkg = argc > 1 ? argv[1] : "../out/modele.a64.rmp";
  const char *native = argc > 2 ? argv[2] : "../libmodele.dylib";
  rmp_info info;
  const rmp_plugin *p = load(pkg, info);
  CHECK(p, "%s does not load", pkg);
  if (!p) return 1;
  printf("load      %s: id %s, image %u B, bss %u B, state %u B, cpu %u permille, flags 0x%x\n", pkg, info.id,
         info.image_size, info.bss_size, p->state_bytes, p->cpu_permille, info.flags);
  CHECK(strcmp(info.id, "modele.voice") == 0, "id %s", info.id);
  CHECK(info.flags & RMP_HF_INSTANCES, "max_voices: flags 0x%x", info.flags);
  CHECK(rmp_hf_pitch(info.flags) != RMP_PITCH_NONE, "pitch: flags 0x%x", info.flags);
  CHECK(p->state_bytes <= 16384u, "state %u over the Teensy's 16 KB (and the Pi's 64 KB)", p->state_bytes);

  // all 36 range + wave pairs on each OSC knob: the triangles' pitch by range, each wave its own, the defaults' pairs, and
  // the render of the defaults as a hash (MD_OSC="84,76,84": the same on a package with 16-name lists: the sound before them)
  {
    float q[16];
    plain(q); osc1_only(q);
    for (int r = 1; r < 6; ++r) {
      q[OSC1] = osc(6 * r);
      std::vector<float> x = hold(p, q, 60, kFs);
      double f = freq_of(x, kFs / 4, x.size()), want = note_hz(60) * std::pow(2.0, r - 3);
      CHECK(std::fabs(cents(f, want)) < 3.0, "OSC1 range %d TRI: %.2f Hz, want %.2f", r, f, want);
    }
    double e[6];
    for (int w = 0; w < 6; ++w) {
      q[OSC1] = osc(18 + w);
      std::vector<float> x = hold(p, q, 60, kFs / 2);
      e[w] = 0;
      for (size_t i = kFs / 4; i + 1 < x.size(); ++i) e[w] += std::fabs(x[i + 1] - x[i]);
    }
    for (int w = 0; w < 6; ++w)
      for (int v = w + 1; v < 6; ++v) CHECK(std::fabs(e[w] - e[v]) > 1e-3 * e[w], "8' waves %d and %d alike", w, v);
    CHECK((72 * 36 >> 7) == 18 + 2 && (56 * 36 >> 7) == 12 + 3, "the defaults: OSC1, OSC2 8 SAW, OSC3 16 SQR");
    float d[16];
    memcpy(d, kDef, sizeof d);
    if (const char *o = getenv("MD_OSC")) sscanf(o, "%f,%f,%f", &d[0], &d[1], &d[2]);
    std::vector<float> x = hold(p, d, 36, kFs);
    uint32_t h = 2166136261u;
    for (float v : x) { uint32_t u; memcpy(&u, &v, 4); h = (h ^ u) * 16777619u; }
    printf("defaults  render hash %08x (note 36, 1 s)\n", h);
  }

  // level: a 16th line on one state (a mono track), the defaults
  {
    Voice v(p);
    v.params(kDef);
    const int step = kFs * 60 / 125 / 4 / kBlock * kBlock;
    const float line[16] = {36, 36, 48, 36, 39, 36, 43, 36, 36, 46, 36, 48, 41, 36, 43, 34};
    std::vector<float> x(16 * step + kFs);
    double lo = 1e9;
    for (int k = 0; k < 16; ++k) {
      v.on(line[k]);
      v.run(x, k * step, k * step + step / 2 / kBlock * kBlock);
      v.off();
      v.run(x, k * step + step / 2 / kBlock * kBlock, (k + 1) * step);
      lo = std::min(lo, rms(x, k * step, k * step + step / 2));
    }
    v.run(x, 16 * step, (int)x.size());
    double tail = rms(x, x.size() - kFs / 10, x.size());
    printf("level     bass line: the quietest step rms %.3f, peak %.3f, whole rms %.3f, tail %.2g\n", lo, peak(x), rms(x, 0, 16 * step), tail);
    CHECK(finite_in(x), "bass: not finite or outside +-1");
    CHECK(lo > 0.05, "bass: a step at rms %.4f", lo);
    CHECK(peak(x) < 0.999f, "bass: clips at the defaults (%.3f)", peak(x));
    CHECK(tail == 0.0, "bass: not silent after the release");
    CHECK(v.guards_ok(), "bass: wrote outside its state");
    for (float n : {24.f, 36.f, 48.f, 60.f, 72.f, 84.f, 96.f}) {
      std::vector<float> y = hold(p, kDef, n, kFs);
      printf("level     note %.0f held: rms %.3f, peak %.3f\n", n, rms(y, kFs / 2, y.size()), peak(y));
      CHECK(peak(y) < 0.999f, "note %.0f: clips (%.3f)", n, peak(y));
      if (n <= 60) CHECK(rms(y, kFs / 2, y.size()) > 0.05, "note %.0f: rms %.4f", n, rms(y, kFs / 2, y.size()));
      else CHECK(peak(y) > 0.1f, "note %.0f: peak %.3f", n, peak(y));   // the contour's sound; the bass patch closes up there
    }
  }

  // pitch
  {
    float q[16];
    double worst = 0;
    for (float n : {36.f, 48.f, 60.f, 72.f, 84.f}) {
      plain(q); osc1_only(q);
      for (int r = 0; r < 2; ++r) {                         // 8 TRI, then 16 TRI (an octave down)
        q[OSC1] = osc(r ? 12 : 18);
        std::vector<float> x = hold(p, q, n, kFs + kFs / 2);
        double f = freq_of(x, kFs / 4, x.size()), want = note_hz(n) * (r ? 0.5 : 1.0), c = cents(f, want);
        worst = std::max(worst, std::fabs(c));
        CHECK(std::fabs(c) < 2.0, "osc 1 note %.0f range %d: %.2f Hz, want %.2f (%.2f cents)", n, r, f, want, c);
      }
      plain(q); osc2_only(q);
      std::vector<float> x = hold(p, q, n, kFs + kFs / 2);
      double f = freq_of(x, kFs / 4, x.size()), c = cents(f, note_hz(n));
      worst = std::max(worst, std::fabs(c));
      CHECK(std::fabs(c) < 2.0, "osc 2 note %.0f: %.2f Hz (%.2f cents)", n, f, c);
    }
    printf("pitch     osc 1, 2 across the keyboard: worst %.3f cents\n", worst);
    // the tuning laws: TUN2 +1 semitone (85), +7 (127); osc 3 alone: TUN3; CONTROL off frees it
    plain(q); osc2_only(q); q[TUN2] = 85;
    std::vector<float> x = hold(p, q, 60, kFs);
    double c1 = cents(freq_of(x, kFs / 4, x.size()), note_hz(61));
    q[TUN2] = 127;
    x = hold(p, q, 60, kFs);
    double c7 = cents(freq_of(x, kFs / 4, x.size()), note_hz(67));
    printf("pitch     TUN2 +1: %.2f cents off, +7: %.2f cents off\n", c1, c7);
    CHECK(std::fabs(c1) < 2.0 && std::fabs(c7) < 2.0, "TUN2: %.2f, %.2f", c1, c7);
    plain(q); q[O3NZ] = 127; q[MIX] = 0; q[TUN3] = 85;       // osc 3 alone, +1 semitone
    x = hold(p, q, 60, kFs);
    double c3 = cents(freq_of(x, kFs / 4, x.size()), note_hz(61));
    CHECK(std::fabs(c3) < 2.0, "TUN3: %.2f", c3);
    q[TUN3] = 64; q[MOD] = 63;                               // CONTROL off, a touch of depth: free, at C2 for 8'
    std::vector<float> a = hold(p, q, 48, kFs), b = hold(p, q, 72, kFs);
    double fa = freq_of(a, kFs / 4, a.size()), fb = freq_of(b, kFs / 4, b.size());
    printf("pitch     osc 3 CONTROL off: note 48 %.2f Hz, note 72 %.2f Hz (C2 %.2f)\n", fa, fb, note_hz(36));
    CHECK(std::fabs(cents(fb, fa)) < 3.0, "CONTROL off still follows the keys: %.2f, %.2f Hz", fa, fb);
    CHECK(std::fabs(cents(fa, note_hz(36))) < 5.0, "CONTROL off, 8': %.2f Hz", fa);
    q[TUN3] = 85;                                            // +1 semitone becomes +5, wide
    a = hold(p, q, 48, kFs);
    double cw = cents(freq_of(a, kFs / 4, a.size()), note_hz(41));
    printf("pitch     osc 3 CONTROL off, TUN3 +1 = +5 semitones: %.2f cents off\n", cw);
    CHECK(std::fabs(cw) < 5.0, "the wide tuning: %.2f cents", cw);
    plain(q); q[OSC3] = osc(O3_LOTRI); q[O3NZ] = 127; q[MOD] = 63; q[TUN3] = 127;   // free LO, its top: 3.856 Hz
    x = hold(p, q, 60, 3 * kFs);
    double lo = freq_of(x, kFs / 2, x.size());
    printf("pitch     free LO at its top: %.3f Hz (table 3.856)\n", lo);
    CHECK(std::fabs(lo - 3.856) < 0.15, "free LO: %.3f Hz", lo);
    // the drift: Analog on stays within its 6 cents (a saw through the mixer; the pitch over short windows)
    plain(q); osc1_only(q); q[MODE] = band(K0 - 8);
    x = hold(p, q, 60, 6 * kFs);
    double dmax = 0;
    for (int w = 0; w < 5; ++w) dmax = std::max(dmax, std::fabs(cents(freq_of(x, (w + 1) * kFs, (w + 1) * kFs + kFs / 2), note_hz(60))));
    printf("pitch     Analog on: drift within %.2f cents over 6 s\n", dmax);
    CHECK(dmax < 9.0, "analog drift %.2f cents", dmax);
  }

  // filter: a small triangle (osc 3, linear) through the ladder against a passband measurement
  {
    float q[16];
    const double fcs[3] = {500.0, 2000.0, 8000.0};
    auto gain = [&](double fc, double f, float emph, int mode, float note) {   // the fundamental's amplitude
      plain(q); q[O3NZ] = 72; q[EMPH] = emph; q[MODE] = band(mode);
      q[CUT] = cut_for(fc);
      std::vector<float> x = hold(p, q, note, kFs);
      return amp_at(x, kFs / 2, x.size(), f);
    };
    for (double fc : fcs) {
      float n_at = (float)(69 + 12 * std::log2(fc / 440)), n_low = n_at - 36, n_oct = n_at + 12;
      if (n_oct > 110) n_oct = n_at + 7;
      double f_oct = note_hz(n_oct), a_low = gain(fc, note_hz(n_low), 0, K0, n_low);
      double a_fc = gain(fc, note_hz(n_at), 0, K0, n_at), a_oct = gain(fc, f_oct, 0, K0, n_oct);
      double want = -40.0 * std::log10(1.0 + (f_oct / fc) * (f_oct / fc));   // four one-poles
      printf("filter    fc %.0f Hz: at fc %.2f dB (want -12), at %.0f Hz %.2f dB (analog %.2f)\n", fc, db(a_fc, a_low), f_oct, db(a_oct, a_low), want);
      CHECK(std::fabs(db(a_fc, a_low) + 12.0) < 2.0, "fc %.0f: %.2f dB at the cutoff", fc, db(a_fc, a_low));
      if (fc <= 2000.0) CHECK(std::fabs(db(a_oct, a_low) - want) < 3.0, "fc %.0f: the slope %.2f vs %.2f dB", fc, db(a_oct, a_low), want);
    }
    // emphasis: the cutoff's gain rises against the passband
    double e0 = gain(1000, 1000, 0, K0, 75.3f), e3 = gain(1000, 1000, 70, K0, 75.3f);
    printf("filter    EMPH 70 at the cutoff: %+.2f dB\n", db(e3, e0));
    CHECK(db(e3, e0) > 6.0, "emphasis adds only %.2f dB at the cutoff", db(e3, e0));
    // keyboard tracking: K3 moves the cutoff an octave an octave (from C4): a note at the cutoff stays at -12 dB
    {
      double fcC4 = 1000.0;
      double a0 = gain(fcC4, note_hz(60 + 12 + 3.86), 0, K3, 75.86f), a1 = gain(fcC4, note_hz(60 + 0 + 3.86), 0, K3, 63.86f);
      double g0 = gain(fcC4 * 2, note_hz(75.86), 0, K0, 75.86f), g1 = gain(fcC4, note_hz(63.86), 0, K0, 63.86f);
      printf("filter    K3: gain at a note a +3.86 st over the cutoff: %.2f dB, an octave up %.2f dB (K0 refs %.2f, %.2f)\n",
             db(a1, 1), db(a0, 1), db(g1, 1), db(g0, 1));
      CHECK(std::fabs(db(a0, g0)) < 1.0 && std::fabs(db(a1, g1)) < 1.0, "tracking: %.2f %.2f dB", db(a0, g0), db(a1, g1));
    }
    // the contour opens the filter and closes it: the note's brightness early against late
    {
      plain(q); osc1_only(q); q[OSC1] = osc(O1_8SAW); q[CUT] = cut_for(200); q[CONT] = 100; q[DEC] = 60; q[SUS] = 64;
      std::vector<float> x = hold(p, q, 48, 2 * kFs);
      double early = amp_at(x, kFs / 100, kFs / 100 + kFs / 20, note_hz(48) * 8), late = amp_at(x, kFs, 2 * kFs - 12, note_hz(48) * 8);
      printf("filter    contour: the 8th harmonic early %.2f dB over late\n", db(early, late));
      CHECK(db(early, late) > 18.0, "the contour moves the 8th harmonic only %.2f dB", db(early, late));
    }
  }

  // self-oscillation: no input at all
  {
    float q[16];
    double worst = 0;
    for (double fc : {110.0, 440.0, 1000.0, 3000.0, 8000.0}) {
      plain(q); q[MIX] = 0; q[O3NZ] = 64; q[EMPH] = 127; q[CUT] = cut_for(fc);
      std::vector<float> x = hold(p, q, 60, 2 * kFs);
      double f = freq_of(x, kFs, x.size()), r = rms(x, kFs, x.size()), c = cents(f, fc);
      size_t rise = 0;
      for (size_t i = 0; i < x.size(); ++i) if (std::fabs(x[i]) > 0.5f * peak(x)) { rise = i; break; }
      worst = std::max(worst, std::fabs(c));
      printf("selfosc   fc %.0f Hz: %.1f Hz (%+.1f cents), rms %.3f, peak %.3f, half level after %.1f ms\n", fc, f, c, r, peak(x), 1000.0 * (double)rise / kFs);
      CHECK(std::fabs(c) < 15.0, "self-oscillation %.1f cents from %.0f Hz", c, fc);
      CHECK(r > 0.05, "no self-oscillation at %.0f Hz (rms %.4f)", fc, r);
      CHECK(rise < (size_t)kFs / 4, "takes %.0f ms to speak at %.0f Hz", 1000.0 * (double)rise / kFs, fc);
      CHECK(finite_in(x), "selfosc: not finite or outside +-1");
    }
    // the threshold: EMPH 110 sustains, 90 rings out
    plain(q); q[MIX] = 0; q[O3NZ] = 64; q[EMPH] = 110; q[CUT] = cut_for(1000);
    std::vector<float> x = hold(p, q, 60, 2 * kFs);
    double r110 = rms(x, kFs, x.size());
    q[EMPH] = 90;
    x = hold(p, q, 60, 2 * kFs);
    double r90 = rms(x, kFs, x.size());
    printf("selfosc   threshold: EMPH 110 rms %.3f, EMPH 90 rms %.2g\n", r110, r90);
    CHECK(r110 > 0.05 && r90 < 1e-3, "threshold: %.4f, %.4f", r110, r90);
    // the keys play it: K3, note +12 = fc x 2
    plain(q); q[MIX] = 0; q[O3NZ] = 64; q[EMPH] = 127; q[CUT] = cut_for(440); q[MODE] = band(K3);
    std::vector<float> a = hold(p, q, 60, 2 * kFs), b = hold(p, q, 72, 2 * kFs);
    double fa = freq_of(a, kFs, a.size()), fb = freq_of(b, kFs, b.size());
    printf("selfosc   K3: note 60 %.1f Hz, note 72 %.1f Hz (%+.1f cents off the octave)\n", fa, fb, cents(fb, 2 * fa));
    CHECK(std::fabs(cents(fb, 2 * fa)) < 10.0, "tracking %.1f cents", cents(fb, 2 * fa));
  }

  // glide: pure osc 1, an octave up mid-note; measured in 5 ms windows
  {
    float q[16];
    plain(q); osc1_only(q); q[GA] = 40;                      // left of the centre: glide
    Voice v(p);
    v.params(q);
    v.on(45);
    std::vector<float> x(kFs * 5 / 2);
    const int at = kFs / 4 / kBlock * kBlock;
    v.run(x, 0, at);
    v.on(57);
    v.run(x, at, (int)x.size());
    double prev = 0, biggest = 0, t63 = 0;
    bool mono = true;
    const int win = kFs / 50;
    for (int i = at + win; i + win < (int)x.size(); i += win / 2) {
      double f = freq_of(x, i, i + win), st = 12 * std::log2(f / 110.0);
      if (prev > 0) { biggest = std::max(biggest, st - prev); if (st < prev - 0.06) mono = false; }
      if (t63 == 0 && st >= 12 * 0.632) t63 = (double)(i + win / 2 - at) / kFs;
      prev = st;
    }
    double f_end = freq_of(x, x.size() - kFs / 4, x.size());
    double tau = (24.0 / 64) * (24.0 / 64) * 1.5;
    printf("glide     a 12 semitone glide: largest move %.2f semitones in 10 ms, 63 %% after %.0f ms (tau %.0f ms), ends %.1f Hz\n", biggest, 1000 * t63, 1000 * tau, f_end);
    CHECK(mono, "the glide goes backwards");
    CHECK(biggest < 2.0, "a jump of %.2f semitones in the glide", biggest);
    CHECK(std::fabs(t63 - tau) < 0.25 * tau, "the glide takes %.0f ms, tau %.0f", 1000 * t63, 1000 * tau);
    CHECK(std::fabs(cents(f_end, 220)) < 5.0, "the glide ends at %.1f Hz", f_end);
    q[GA] = 64;
    Voice w(p);
    w.params(q);
    w.on(45);
    w.run(x, 0, at);
    w.on(57);
    w.run(x, at, (int)x.size());
    double fj = freq_of(x, at + kFs / 200, at + kFs / 200 + 1200);
    printf("glide     G<>A centre: %.1f Hz 5 ms after the note\n", fj);
    CHECK(std::fabs(cents(fj, 220)) < 30.0, "no glide: %.1f Hz", fj);
  }

  // click: a slow triangle at a high level, so the wave's own step is small; 110 Hz: 0.9 % of the peak a sample
  {
    float q[16];
    plain(q); osc1_only(q);
    Voice v(p);
    v.params(q);
    std::vector<float> x(kFs);
    const int on = kFs / 4 / kBlock * kBlock, offt = kFs / 2 / kBlock * kBlock;
    v.run(x, 0, on);
    v.on(45);
    v.run(x, on, offt);
    float pk = peak(x), steady = max_step(x, on + kFs / 8, offt);
    float son = max_step(x, on, on + kFs / 50);
    v.off();
    v.run(x, offt, (int)x.size());
    float soff = max_step(x, offt - 12, offt + kFs / 50), tail = (float)rms(x, offt + kFs / 25, x.size());
    printf("click     peak %.3f, steady step %.4f, note on %.4f, note off %.4f, tail %.2g\n", pk, steady, son, soff, tail);
    CHECK(son < 0.08f * pk, "a note-on step of %.4f (peak %.3f)", son, pk);
    CHECK(soff < 0.08f * pk, "a note-off step of %.4f (peak %.3f)", soff, pk);
    CHECK(tail == 0.0f, "the release leaves %.3g", tail);
    // retrigger and steal: the defaults, a sounding note, its largest step against the first attack's
    for (int mode = 0; mode < 2; ++mode) {
      Voice r(p);
      r.params(kDef);
      r.on(48);
      std::vector<float> y(kFs);
      const int at = kFs / 2 / kBlock * kBlock;
      r.run(y, 0, at);
      r.on(mode ? 55.f : 48.f);                              // a stolen voice takes another note
      r.run(y, at, (int)y.size());
      float first = max_step(y, 0, kFs / 10), around = max_step(y, at - 48, at + kFs / 10);
      printf("click     %s: largest step %.3f (the first attack's %.3f)\n", mode ? "steal" : "retrigger", around, first);
      CHECK(around <= (mode ? 2.0f : 1.5f) * first + 1e-3f, "%s: a step of %.3f (first %.3f)", mode ? "steal" : "retrigger", around, first);
    }
    // a fast CUT and a full EMPH sweep under a held triangle: smooth; the step against the steady wave's
    plain(q); osc1_only(q); q[CUT] = 30;
    Voice s(p);
    s.params(q);
    s.on(45);
    std::vector<float> y(kFs * 2);
    for (int i = 0; i + kBlock <= (int)y.size(); i += kBlock) {
      q[CUT] = 70.0f + 57.0f * (float)std::sin(2 * M_PI * 4.0 * i / kFs);
      q[EMPH] = 40.0f + 30.0f * (float)std::sin(2 * M_PI * 3.0 * i / kFs);
      s.params(q);
      p->render(s.st, &y[i], kBlock);
    }
    printf("click     CUT / EMPH sweeps at 4 Hz: largest step %.4f, peak %.3f\n", max_step(y, kFs / 2, y.size()), peak(y));
    CHECK(max_step(y, kFs / 2, y.size()) < 0.12f * std::max(peak(y), 0.05f), "a sweep steps by %.4f", max_step(y, kFs / 2, y.size()));
  }

  // mod: osc 3 modulates osc 1, silent at its own level
  {
    float q[16];
    plain(q); osc1_only(q); q[OSC3] = osc(O3_LOTRI); q[MOD] = 0; q[TUN3] = 127;   // left: free, full depth; 3.9 Hz
    std::vector<float> x = hold(p, q, 60, 3 * kFs);
    double lo = 1e9, hi = 0;
    for (int i = kFs / 2; i + 2400 < (int)x.size(); i += 1200) {
      double f = freq_of(x, i, i + 2400);
      lo = std::min(lo, f); hi = std::max(hi, f);
    }
    printf("mod       osc 3 LO triangle, depth 1 (1.51 octaves): osc 1 swings %.1f .. %.1f Hz (%.2f octaves)\n", lo, hi, std::log2(hi / lo));
    CHECK(std::log2(hi / lo) > 1.0 && std::log2(hi / lo) < 2.2, "the swing is %.2f octaves", std::log2(hi / lo));
    q[MOD] = 90; q[OSC3] = osc(O3_8SAW); q[TUN3] = 70;      // audio rate, CONTROL on
    x = hold(p, q, 48, kFs);
    printf("mod       audio rate: rms %.3f, peak %.3f\n", rms(x, kFs / 4, x.size()), peak(x));
    CHECK(finite_in(x) && rms(x, kFs / 4, x.size()) > 0.02, "audio-rate modulation");
  }

  // extreme: all the corners
  {
    float clip_most = 0;
    uint32_t rs = 12345;
    for (int mode = 0; mode < 3; ++mode) {
      Voice v(p);
      float prm[16];
      for (int i = 0; i < 16; ++i) prm[i] = mode == 0 ? 0.0f : 127.0f;
      v.params(mode == 2 ? kDef : prm);
      std::vector<float> x(kFs);
      v.on(mode == 1 ? 127.0f : 24.0f);
      for (int i = 0; i + kBlock <= kFs; i += kBlock) {
        if (mode == 2) {
          for (int j = 0; j < 16; ++j) prm[j] = (float)(hrand(&rs) % 128) + (float)(hrand(&rs) % 128) / 128.0f;
          v.params(prm);
          if (i % 4800 == 0) v.on((float)(hrand(&rs) % 128));
          if (i % 7200 == 0) v.off();
        }
        p->render(v.st, &x[i], kBlock);
      }
      CHECK(finite_in(x), "knobs mode %d: not finite or outside +-1", mode);
      CHECK(v.guards_ok(), "knobs mode %d: wrote outside its state", mode);
      printf("extreme   %s: rms %.3f, finite\n", mode == 0 ? "all 0" : mode == 1 ? "all 127" : "random each block", rms(x, 0, x.size()));
    }
    for (int k = 0; k < 16; ++k)
      for (int end = 0; end < 2; ++end)
        for (float n : {12.f, 60.f, 120.f}) {
          float prm[16];
          memcpy(prm, kDef, sizeof prm);
          prm[k] = end ? 127.0f : 0.0f;
          Voice v(p);
          v.params(prm);
          v.on(n);
          std::vector<float> x(kFs / 2 / kBlock * kBlock);
          v.run(x, 0, (int)x.size() / 2);
          v.off();
          v.run(x, (int)x.size() / 2, (int)x.size());
          CHECK(finite_in(x), "P%d at %d, note %.0f: not finite or outside +-1", k + 1, end ? 127 : 0, n);
          CHECK(v.guards_ok(), "P%d at %d: wrote outside its state", k + 1, end ? 127 : 0);
        }
    // EMPH max, all three loud, fast modulation, every note, a cutoff sweep: the worst the patch can do
    size_t clipped = 0, total = 0;
    float worst = 0;
    for (int note = 0; note < 128; note += 7) {
      float prm[16];
      memcpy(prm, kDef, sizeof prm);
      prm[OSC3] = osc(O3_8SAW); prm[O3NZ] = 127; prm[MOD] = 127; prm[EMPH] = 127; prm[CONT] = 127; prm[DEC] = 20;
      Voice v(p);
      v.params(prm);
      v.on((float)note);
      std::vector<float> x(kFs / 2 / kBlock * kBlock);
      for (int i = 0; i + kBlock <= (int)x.size(); i += kBlock) {
        prm[CUT] = 64.0f + 63.0f * (float)std::sin(2 * M_PI * 30.0 * i / kFs);
        v.params(prm);
        p->render(v.st, &x[i], kBlock);
      }
      CHECK(finite_in(x), "EMPH max, fast mod, note %d", note);
      CHECK(v.guards_ok(), "EMPH max: wrote outside its state");
      for (float s : x) { worst = std::max(worst, std::fabs(s)); if (std::fabs(s) >= 1.0f) ++clipped; ++total; }
    }
    clip_most = 100.0f * (float)clipped / (float)total;
    printf("extreme   EMPH max, modulation, cutoff sweep across the keyboard: peak %.3f, %.2f %% of samples at the clamp\n", worst, clip_most);
    CHECK(clip_most < 15.0f, "%.2f %% at the clamp", clip_most);
    // LEV at the top on the loudest sustained sound
    float prm[16];
    memcpy(prm, kDef, sizeof prm);
    prm[OSC3] = osc(O3_8SAW); prm[O3NZ] = 127; prm[LEV] = 127; prm[CUT] = 127; prm[SUS] = 127;
    std::vector<float> x = hold(p, prm, 48, kFs);
    printf("extreme   three saws, filter open, LEV 127: peak %.3f, rms %.3f\n", peak(x), rms(x, kFs / 2, x.size()));
  }

  // denormal: a held note with nothing going in; a long tail
  {
    float q[16];
    plain(q); q[MIX] = 0; q[O3NZ] = 64; q[CUT] = 30;
    Voice v(p);
    v.params(q);
    v.on(60);
    std::vector<float> x(10 * kFs / kBlock * kBlock);
    v.run(x, 0, (int)x.size());
    size_t sub = 0;
    for (float s : x) if (s != 0.0f && std::fabs(s) < FLT_MIN) ++sub;
    double silent = time_voice(p, q), playing = time_voice(p, kDef);
    printf("denormal  10 s of an empty ladder: %zu subnormal samples; %.0f ns a block silent, %.0f playing\n", sub, silent, playing);
    CHECK(sub == 0, "%zu subnormal samples", sub);
    CHECK(silent < 1.5 * playing, "a silent ladder takes %.0f ns (playing %.0f)", silent, playing);
    float t[16];
    memcpy(t, kDef, sizeof t);
    t[DEC] = 127; t[MODE] = band(4 + 2);                     // the DECAY switch on: a 20 s release
    Voice w(p);
    w.params(t);
    w.on(48);
    std::vector<float> y(23 * kFs / kBlock * kBlock);   // -40 dB in 10 s, the end at -80
    w.run(y, 0, kFs / kBlock * kBlock);
    w.off();
    w.run(y, kFs / kBlock * kBlock, (int)y.size());
    sub = 0;
    for (float s : y) if (s != 0.0f && std::fabs(s) < FLT_MIN) ++sub;
    printf("denormal  a 20 s release: %zu subnormal samples, ends %.2g\n", sub, rms(y, y.size() - kFs / 4, y.size()));
    CHECK(sub == 0, "%zu subnormal samples in the tail", sub);
    CHECK(rms(y, y.size() - kFs / 4, y.size()) == 0.0, "the release does not end");
  }

  // chunks: other block sizes sound the same
  {
    double ref = 0;
    for (int n : {12, 7, 4, 1}) {
      Voice v(p);
      float q[16];
      plain(q); osc1_only(q);
      q[OSC1] = osc(O1_8SAW); q[CUT] = 60; q[EMPH] = 60;
      v.params(q);
      v.on(48);
      std::vector<float> x(kFs);
      for (int i = 0; i + n <= kFs; i += n) p->render(v.st, &x[i], n);
      double r = rms(x, kFs / 2, kFs);
      if (n == 12) ref = r;
      printf("chunks    render(%d): rms %.4f\n", n, r);
      CHECK(std::fabs(db(r, ref)) < 0.5, "render(%d) differs by %.2f dB", n, db(r, ref));
      CHECK(finite_in(x), "render(%d)", n);
    }
  }

  // cpu
  {
    float saw[16], top[16];
    memcpy(saw, kDef, sizeof saw);
    saw[OSC3] = osc(O3_8SAW); saw[O3NZ] = 127; saw[EMPH] = 100; saw[CUT] = 90; saw[SUS] = 127;
    memcpy(top, saw, sizeof top);
    top[MOD] = 127;
    float dry[16];
    memcpy(dry, kDef, sizeof dry);
    dry[MODE] = band(K2);                                    // Analog off
    double m = time_voice(p, kDef), m1 = time_voice(p, dry), m2 = time_voice(p, saw), m3 = time_voice(p, top), m4 = time_voice(p, top, 96.0f);
    printf("cpu       package: %.0f ns a 12-frame call (defaults), %.0f (Analog off), %.0f (three saws, EMPH), %.0f (+ modulation), %.0f (the same at note 96)\n", m, m1, m2, m3, m4);
    // the bank's cost grows with the edges: three narrow pulses, two edges a cycle each, up the keyboard
    float edge[16];
    memcpy(edge, top, sizeof edge);
    edge[OSC1] = osc(23); edge[OSC2] = osc(27); edge[OSC3] = osc(21);   // 8 NAR, 4 SQR, 8 SQR
    for (float n : {60.f, 84.f, 96.f, 108.f, 120.f})
      printf("cpu       edges: note %.0f: %.0f ns\n", n, time_voice(p, edge, n));
    void *hn = dlopen(native, RTLD_NOW);
    const rmp_plugin *pn = hn ? (const rmp_plugin *)dlsym(hn, "rmp_entry") : nullptr;
    CHECK(pn && pn->init(&g_host) == 0, "%s: no native build", native);
    if (pn) {
      std::vector<double> est;
      double mn = time_voice(pn, kDef), mn3 = time_voice(pn, top, 96.0f);
      printf("cpu       native: %.0f ns (defaults), %.0f (worst patch)\n", mn, mn3);
      for (int r = 3; r < argc; ++r) {
        void *h = dlopen(argv[r], RTLD_NOW);
        const rmp_plugin *pr = h ? (const rmp_plugin *)dlsym(h, "rmp_entry") : nullptr;
        if (!pr || pr->init(pr->api == 1u ? &g_host1 : &g_host) != 0) { printf("cpu       (%s: not loaded)\n", argv[r]); continue; }
        float rp[16];
        for (int i = 0; i < 16; ++i) rp[i] = 64.0f;
        rp[1] = 100.0f;
        double mr = time_voice(pr, rp);
        printf("cpu       %s: %.0f ns (declared %u permille): modele x %.2f defaults, x %.2f worst patch, estimate %.0f..%.0f permille\n",
               strrchr(argv[r], '/') + 1, mr, pr->cpu_permille, mn / mr, mn3 / mr, pr->cpu_permille * mn / mr, pr->cpu_permille * mn3 / mr);
        est.push_back(pr->cpu_permille * mn3 / mr);
      }
      if (!est.empty()) {
        std::sort(est.begin(), est.end());
        double e = est[est.size() / 2];
        printf("cpu       estimate %.0f permille (median, the worst patch), declared %u\n", e, p->cpu_permille);
        CHECK(e <= 2.0 * p->cpu_permille, "the estimate %.0f is over the guard's 2x of the declared %u", e, p->cpu_permille);
      }
    }
    double budget_ns = 1e9 * kBlock / kFs;
    printf("cpu       Mac x 34: %.2f %% of a Pi 4 core a voice (defaults), %.2f %% (worst patch)\n", 100 * m * 34 / budget_ns, 100 * m4 * 34 / budget_ns);
  }

  printf(g_fail ? "md_check: %d FAILED\n" : "md_check: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
