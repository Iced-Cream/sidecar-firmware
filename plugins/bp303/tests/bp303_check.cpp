// plugins/bp303/tests/bp303_check.cpp - the host checks of BP303: the RMPA package (make package: out/bp303.a64.rmp)
// loaded and run on this arm64 Mac as the Pi runs it (rmp_loader: verify, the two parts placed, relocated; MAP_JIT):
//   load      verifies, places, init()s; a pitched mono machine; the state fits the Teensy's 16 KB
//   rest      no note: exact silence; a note, then note_off: exact silence within 100 ms, nothing written outside the
//             state (guard bytes); 3 s later (the voice asleep) still silent, and the next note sounds
//   (tests/bp303_state.c has the checks that need the voice's state: subnormals inside it, 1-frame calls, VOL 64 / 65)
//   sweep     an accented line at the default VOL over CUT x RES (each 0 .. 127 in 9 steps), saw and square: finite, no
//             subnormal, under the ceiling (the soft drive: never 1.0)
//   long      a note held 30 s at DEC 0: finite, no subnormal output, silent once it has died away; a trig at 29 s (the
//             gate still open) starts a new note; silent after the release
//   knobs     every P at 0, at 127, and random values each block mid-note: finite, within +-1
//   slide     C3, then an overlapping C4 (a bare saw, CUT 127): the pitch glides (SLID 73) or jumps (SLID 0) and ends
//             on C4, with no retrigger (the level does not jump up, as it does for a new note after note_off); one
//             note_off then ends the one note
//   accent    velocity 127 is louder than velocity 64, and brighter (on a closed, still filter); velocity 100 is not an accent (sample-identical to
//             64); ACC 0 turns accents off
//   cpu       ns per 12-frame render call (the best of 5 means) a voice, playing, saw and square; the native build
//             against sine_test's, sc_acid's and sc_wavepad's (their declared permille)
// MIT, Copyright 2026 trigcondition and contributors.
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

#include <algorithm>
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

// the package, placed as the Pi's loader places it (mdkit's mdk_check.cpp: RW data right after the code)
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

// one voice, its state between guard bytes
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
  void block(float *o) { p->render(st, o, kBlock); }
};

// the manifest's defaults (manifest.json; a P without a knob 64)
static const float kDef[16] = {44, 64, 32, 72, 100, 0, 73, 99, 64, 64, 64, 64, 64, 64, 64, 64};
enum { P_CUT, P_RES, P_ENV, P_DEC, P_ACC, P_WAVE, P_SLID, P_VOL, P_TUNE };

static double rms(const std::vector<float> &x, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < b && i < x.size(); ++i) s += (double)x[i] * x[i];
  return b > a ? std::sqrt(s / (double)(b - a)) : 0.0;
}
static float peak(const std::vector<float> &x) {
  float pk = 0;
  for (float v : x) pk = std::max(pk, std::fabs(v));
  return pk;
}
static bool finite_in(const std::vector<float> &x) {
  for (float v : x) if (!std::isfinite(v) || v < -1.0f || v > 1.0f) return false;
  return true;
}
static bool no_subnormal(const std::vector<float> &x) {
  for (float v : x) if (std::fpclassify(v) == FP_SUBNORMAL) return false;
  return true;
}
// the energy above hz as a share of the whole (a one-pole high-pass: a brightness figure)
static double bright(const std::vector<float> &x, size_t a, size_t b, double hz) {
  double k = std::exp(-2.0 * M_PI * hz / kFs), lp = 0, hi = 0, all = 0;
  for (size_t i = a; i < b; ++i) { lp = x[i] + k * (lp - x[i]); double h = x[i] - lp; hi += h * h; all += (double)x[i] * x[i]; }
  return all > 0 ? hi / all : 0.0;
}

// a 16-step line at 125 BPM: (note, accent, slides into the next); gate 58 % of a step, a slide's note held into the next
struct Step { float note; bool acc, slide; };
static const Step kLine[16] = {{36, 0, 0}, {36, 0, 0}, {48, 1, 0}, {36, 0, 1}, {39, 0, 0}, {36, 1, 0}, {0, 0, 0}, {46, 1, 0},
                               {36, 0, 0}, {48, 0, 1}, {36, 1, 0}, {36, 0, 0}, {43, 0, 1}, {41, 1, 0}, {36, 0, 0}, {34, 1, 0}};
static std::vector<float> play_line(const rmp_plugin *p, const float *prm, int bars, Voice *keep = nullptr) {
  Voice local(p);
  Voice &v = keep ? *keep : local;
  v.params(prm);
  const int step = 5760 / kBlock * kBlock, gate = step * 58 / 100 / kBlock * kBlock;
  std::vector<float> x((size_t)bars * 16 * step + kFs / 2);
  int off = -1;
  for (int i = 0; i < (int)x.size(); i += kBlock) {
    if (i == off) p->note_off(v.st);
    if (i % step == 0 && i / step < bars * 16) {
      const Step &s = kLine[(i / step) % 16];
      if (s.note > 0) {
        p->note_on(v.st, s.note, s.acc ? 1.0f : 0.5f);
        off = i + (s.slide ? step + gate : gate);          // (a slide's next note sets its own)
      }
    }
    v.block(&x[i]);
  }
  return x;
}

// ns per render call while notes play: the best mean of five runs of 20000 calls after a warm-up
static double time_voice(const rmp_plugin *p, const float *prm) {
  Voice v(p);
  v.params(prm);
  p->note_on(v.st, 48.0f, 0.8f);
  float o[kBlock];
  for (int i = 0; i < 2000; ++i) v.block(o);
  const int blocks = 20000;
  double mean = 1e30;
  for (int run = 0; run < 5; ++run) {   // the least disturbed of five runs
    double t0 = now_ns();
    for (int i = 0; i < blocks; ++i) {
      if (i % 500 == 0) { p->note_off(v.st); p->note_on(v.st, 48.0f, 0.8f); }   // a new note every 125 ms: never asleep
      v.block(o);
    }
    mean = std::min(mean, (now_ns() - t0) / blocks);
  }
  return mean;
}

int main(int argc, char **argv) {
  const char *pkg = argc > 1 ? argv[1] : "../out/bp303.a64.rmp";
  const char *native = argc > 2 ? argv[2] : "../libbp303.dylib";
  rmp_info info;
  const rmp_plugin *p = load(pkg, info);
  CHECK(p, "%s does not load", pkg);
  if (!p) return 1;
  printf("load      %s: id %s, image %u B, bss %u B, state %u B, cpu %u permille, flags 0x%x\n", pkg, info.id,
         info.image_size, info.bss_size, p->state_bytes, p->cpu_permille, info.flags);
  CHECK(strcmp(info.id, "bp303.bass") == 0, "id %s", info.id);
  CHECK(rmp_hf_pitch(info.flags) != RMP_PITCH_NONE, "pitch: flags 0x%x", info.flags);
  CHECK(!(info.flags & RMP_HF_INSTANCES), "a mono machine: flags 0x%x", info.flags);
  CHECK(p->state_bytes <= 16384u, "state %u over the Teensy's 16 KB (and the Pi's 64 KB)", p->state_bytes);

  {   // rest
    Voice v(p);
    v.params(kDef);
    std::vector<float> x(8 * kFs / kBlock * kBlock);
    const int on = kFs, off = on + kFs / 4, again = off + 3 * kFs;
    for (int i = 0; i < (int)x.size(); i += kBlock) {
      if (i == on / kBlock * kBlock || i == again / kBlock * kBlock) p->note_on(v.st, 36.0f, 0.5f);
      if (i == off / kBlock * kBlock || i == (again + kFs / 4) / kBlock * kBlock) p->note_off(v.st);
      v.block(&x[i]);
    }
    double before = rms(x, 0, on - kBlock), held = rms(x, on + kFs / 50, off - kFs / 50);
    double after = rms(x, off + kFs / 10, again - kBlock), second = rms(x, again + kFs / 50, again + kFs / 5);
    printf("rest      rms before %.3g, held %.3f, 100 ms after note_off %.3g, the next note (3 s later) %.3f\n", before, held,
           after, second);
    CHECK(before == 0.0, "sound before any note (%.3g)", before);
    CHECK(held > 0.02, "rms %.4f while held", held);
    CHECK(after == 0.0, "not silent after the release (%.3g)", after);
    CHECK(second > 0.02 && std::fabs(second / held - 1.0) < 0.5, "the note after the sleep: rms %.4f (the first %.4f)", second, held);
    CHECK(finite_in(x) && no_subnormal(x), "rest: not finite, outside +-1 or subnormal");
    CHECK(v.guards_ok(), "rest: wrote outside its state");
  }

  {   // sweep: CUT x RES at the default VOL, saw and square
    float worst = 0;
    int wc = 0, wr = 0, ww = 0, bad = 0;
    for (int wave = 0; wave <= 127; wave += 127)
      for (int c = 0; c < 9; ++c)
        for (int r = 0; r < 9; ++r) {
          float prm[16];
          memcpy(prm, kDef, sizeof prm);
          prm[P_CUT] = c * 127.0f / 8; prm[P_RES] = r * 127.0f / 8; prm[P_WAVE] = (float)wave; prm[P_ENV] = 100;
          std::vector<float> x = play_line(p, prm, 1);
          float pk = peak(x);
          if (pk > worst) { worst = pk; wc = (int)prm[P_CUT]; wr = (int)prm[P_RES]; ww = wave; }
          if (!finite_in(x) || !no_subnormal(x) || pk >= 0.9999f || rms(x, 0, x.size()) < 1e-3) ++bad;
        }
    printf("sweep     162 settings (CUT x RES, saw and square, ENV 100, VOL 99): highest peak %.4f (CUT %d RES %d WAVE %d)\n",
           worst, wc, wr, ww);
    CHECK(bad == 0, "%d settings not finite, subnormal, silent or at the ceiling", bad);
  }

  {   // long: 30 s held at DEC 0
    Voice v(p);
    float prm[16];
    memcpy(prm, kDef, sizeof prm);
    prm[P_DEC] = 0;
    v.params(prm);
    p->note_on(v.st, 36.0f, 1.0f);
    std::vector<float> x(31 * kFs / kBlock * kBlock);
    for (int i = 0; i < (int)x.size(); i += kBlock) {
      if (i == 29 * kFs / kBlock * kBlock) p->note_on(v.st, 36.0f, 0.5f);   // the gate still open: the note has died away
      if (i == 30 * kFs / kBlock * kBlock) p->note_off(v.st);
      v.block(&x[i]);
    }
    double died = rms(x, 28 * kFs, 29 * kFs), late = rms(x, 29 * kFs, 29 * kFs + kFs / 5), tail = rms(x, x.size() - kFs / 2, x.size());
    printf("long      held 30 s: rms the first second %.3f, the 29th %.2g, a trig at 29 s %.3f, after the release %.2g\n",
           rms(x, 0, kFs), died, late, tail);
    CHECK(died == 0.0, "long: the held note has not died away to silence (%.3g)", died);
    CHECK(late > 0.02, "long: a trig on a held note that has died away does not sound (%.4f)", late);
    CHECK(finite_in(x) && no_subnormal(x), "long: not finite, outside +-1 or subnormal");
    CHECK(tail == 0.0, "long: not silent after the release (%.3g)", tail);
    CHECK(v.guards_ok(), "long: wrote outside its state");
  }

  // knobs: all 0, all 127, random each block
  for (int mode = 0; mode < 3; ++mode) {
    Voice v(p);
    float prm[16];
    for (int i = 0; i < 16; ++i) prm[i] = mode == 0 ? 0.0f : 127.0f;
    v.params(mode == 2 ? kDef : prm);
    uint32_t rs = 12345;
    std::vector<float> x(2 * kFs / kBlock * kBlock);
    p->note_on(v.st, mode == 1 ? 127.0f : 24.0f, 1.0f);
    for (int i = 0; i < (int)x.size(); i += kBlock) {
      if (mode == 2) {
        for (int j = 0; j < 16; ++j) prm[j] = (float)(hrand(&rs) % 128) + (float)(hrand(&rs) % 128) / 128.0f;
        v.params(prm);
        if (i % 4800 == 0) p->note_on(v.st, (float)(hrand(&rs) % 128), (hrand(&rs) & 1) ? 1.0f : 0.5f);
        if (i % 14400 == 7200) p->note_off(v.st);
      }
      v.block(&x[i]);
    }
    CHECK(finite_in(x) && no_subnormal(x), "knobs mode %d: not finite, outside +-1 or subnormal", mode);
    CHECK(v.guards_ok(), "knobs mode %d: wrote outside its state", mode);
    printf("knobs     %s: rms %.3f, peak %.3f, finite\n", mode == 0 ? "all 0" : mode == 1 ? "all 127" : "random", rms(x, 0, x.size()),
           peak(x));
  }
  {   // NaN knobs and notes do not get out
    Voice v(p);
    float prm[16];
    for (int i = 0; i < 16; ++i) prm[i] = NAN;
    v.params(prm);
    p->note_on(v.st, NAN, NAN);
    std::vector<float> x(kFs / kBlock * kBlock);
    for (int i = 0; i < (int)x.size(); i += kBlock) v.block(&x[i]);
    CHECK(finite_in(x), "NaN knobs: not finite or outside +-1");
  }

  // slide: a bare saw (CUT 127, RES 0, ENV 0, clean VOL), C3, then C4 while the gate is open / after note_off
  double lvl[3] = {0, 0, 0};
  for (int mode = 0; mode < 3; ++mode) {   // 0: SLID 73 overlapping, 1: SLID 0 overlapping, 2: a new note after note_off
    Voice v(p);
    float prm[16];
    memcpy(prm, kDef, sizeof prm);
    prm[P_CUT] = 127; prm[P_RES] = 0; prm[P_ENV] = 0; prm[P_VOL] = 64; prm[P_SLID] = mode == 1 ? 0.0f : 73.0f;
    v.params(prm);
    p->note_on(v.st, 48.0f, 0.5f);
    const int at = kFs / kBlock * kBlock, end = at + kFs / 2, total = (end + kFs / 4) / kBlock * kBlock;
    std::vector<float> x(total);
    for (int i = 0; i < total; i += kBlock) {
      if (mode == 2 && i == at - 20 * kBlock) p->note_off(v.st);
      if (i == at) p->note_on(v.st, 60.0f, 0.5f);
      if (i == end / kBlock * kBlock) p->note_off(v.st);
      v.block(&x[i]);
    }
    auto hz = [&](int a, int n) { int z = 0; for (int i = a + 1; i < a + n; ++i) z += (x[i - 1] < 0) != (x[i] < 0); return z * 0.5 * kFs / n; };
    double before = hz(at - kFs / 5, kFs / 10), early = hz(at, kFs / 50), late = hz(end - kFs / 5, kFs / 5);
    double l0 = rms(x, at - kFs / 4, at - kFs / 8), l1 = rms(x, at + kFs / 8, at + kFs / 4), tail = rms(x, end + kFs / 10, total);
    lvl[mode] = l1 / l0;
    printf("slide     %s: %.0f Hz before, %.0f Hz the first 20 ms, %.0f Hz at the end; level after / before %.2f; tail %.2g\n",
           mode == 0 ? "SLID 73, overlapping" : mode == 1 ? "SLID 0, overlapping" : "after note_off (a new note)", before, early,
           late, lvl[mode], tail);
    CHECK(std::fabs(before - 130.8) < 8 && std::fabs(late - 261.6) < 8, "mode %d: %.0f / %.0f Hz", mode, before, late);
    if (mode == 0) CHECK(early < 235 && early > 140, "SLID 73: %.0f Hz the first 20 ms (no glide)", early);
    else CHECK(early > 240, "mode %d: %.0f Hz the first 20 ms (a glide)", mode, early);
    if (mode < 2) CHECK(lvl[mode] < 1.0, "mode %d: the level rose across the slide (%.2f): a retrigger", mode, lvl[mode]);
    CHECK(tail == 0.0, "mode %d: one note_off did not end the note (%.3g)", mode, tail);
  }
  CHECK(lvl[2] > 1.1 && lvl[2] > 1.2 * lvl[0], "a new note after note_off does not restart the VCA (%.2f, a slide %.2f)", lvl[2], lvl[0]);

  {   // accent
    auto one = [&](float vel, float accKnob, bool dark = false) {
      Voice v(p);
      float prm[16];
      memcpy(prm, kDef, sizeof prm);
      prm[P_ACC] = accKnob;
      if (dark) { prm[P_CUT] = 30; prm[P_ENV] = 0; prm[P_RES] = 40; }   // a closed, still filter: only an accent moves it
      v.params(prm);
      p->note_on(v.st, 36.0f, vel);
      std::vector<float> x(kFs / 2 / kBlock * kBlock);
      for (int i = 0; i < (int)x.size(); i += kBlock) v.block(&x[i]);
      return x;
    };
    std::vector<float> n64 = one(64.0f / 127.0f, 100), n100 = one(100.0f / 127.0f, 100), a127 = one(1.0f, 100), a0 = one(1.0f, 0);
    std::vector<float> a101 = one(101.0f / 127.0f, 100);
    std::vector<float> dn = one(64.0f / 127.0f, 127, true), da = one(1.0f, 127, true);
    const size_t w = kFs / 10, wb = kFs / 20;
    double rn = rms(n64, 0, w), ra = rms(a127, 0, w), bn = bright(dn, 0, wb, 800), ba = bright(da, 0, wb, 800);
    printf("accent    the first 100 ms (bright: CUT 30, ENV 0, ACC 127, 50 ms, the share above 800 Hz): velocity 64 rms %.3f bright %.3f, velocity 127 rms %.3f bright %.3f\n", rn, bn, ra, ba);
    CHECK(ra > 1.3 * rn, "an accent is not louder (%.3f against %.3f)", ra, rn);
    CHECK(ba > 1.3 * bn, "an accent is not brighter (%.3f against %.3f)", ba, bn);
    CHECK(n100 == n64, "velocity 100 differs from velocity 64 (it is not an accent)");
    CHECK(a101 == a127, "velocity 101 differs from velocity 127 (both accents, by the ACC knob)");
    CHECK(a0 == one(64.0f / 127.0f, 0), "ACC 0: a velocity 127 note differs from a plain one");
  }

  // cpu: the package (the Pi's code), then the native build against the starter machines' declared figures
  float sqr[16];
  memcpy(sqr, kDef, sizeof sqr);
  sqr[P_WAVE] = 127; sqr[P_RES] = 127;
  double m = time_voice(p, kDef), m2 = time_voice(p, sqr);
  printf("cpu       package: %.0f ns mean a 12-frame call (defaults, saw), %.0f (square, RES 127)\n", m, m2);
  void *hn = dlopen(native, RTLD_NOW);
  const rmp_plugin *pn = hn ? (const rmp_plugin *)dlsym(hn, "rmp_entry") : nullptr;
  CHECK(pn && pn->init(&g_host) == 0, "%s: no native build", native);
  if (pn) {
    double mn = time_voice(pn, sqr);
    std::vector<double> est;
    for (int r = 3; r < argc; ++r) {
      void *h = dlopen(argv[r], RTLD_NOW);
      const rmp_plugin *pr = h ? (const rmp_plugin *)dlsym(h, "rmp_entry") : nullptr;
      if (!pr || pr->init(pr->api == 1u ? &g_host1 : &g_host) != 0) { printf("cpu       (%s: not loaded)\n", argv[r]); continue; }
      float rp[16];
      for (int i = 0; i < 16; ++i) rp[i] = 64.0f;
      rp[1] = 100.0f;
      double mr = time_voice(pr, rp);
      est.push_back(pr->cpu_permille * mn / mr);
      printf("cpu       native: bp303 %.0f ns, %s %.0f ns (declared %u): ~%.0f permille\n", mn, strrchr(argv[r], '/') + 1,
             mr, pr->cpu_permille, est.back());
    }
    if (!est.empty()) {
      std::sort(est.begin(), est.end());
      double e = est[est.size() / 2];
      printf("cpu       estimate %.0f permille (median), declared %u\n", e, p->cpu_permille);
      CHECK(e <= 2.0 * p->cpu_permille, "the estimate %.0f is over the guard's 2x of the declared %u", e, p->cpu_permille);
    }
  }
  printf(g_fail ? "bp303_check: %d FAILED\n" : "bp303_check: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
