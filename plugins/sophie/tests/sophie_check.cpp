// plugins/sophie/tests/sophie_check.cpp - the host checks of SOPHIE: the RMPA package (make package:
// out/sophie.a64.rmp) loaded and run on this arm64 Mac as the Pi runs it (rmp_loader: verify, the two parts placed,
// relocated; MAP_JIT):
//   load      verifies, places, init()s; the header's flags opt into instancing (max_voices 8, pitch note); the
//             state is sophie.h's and fits the Pi's 64 KB (and the Teensy's 16 KB)
//   models    each of the 8 MODL entries at notes 36, 60 and 96: sounds, finite, within +-1, no denormal in the
//             output or the state's floats, nothing written outside its state (guard bytes), exact silence once
//             the hit has decayed
//   knobs     each P alone at 0 and at 127 (the others at the defaults), every P at 0, every P at 127, and random
//             values each block with a hit every 100 ms: the same checks
//   overlap   a second hit leaves the first ringing (no jump where it starts); a third hit fades the oldest out
//             (no step larger than the first attack's)
//   cpu       ns per 12-frame render call (the best of 5 means) a voice: the kick at the defaults, the Stack crash
//             with CRSH and the ring, two hits at once; the native build against sine_test's, sc_acid's and
//             sc_wavepad's (their declared permille): the estimate
// MIT, Copyright 2026 trigcondition and contributors.
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rmp_api.h"
#include "rmp_loader.h"
#include "../sophie.h"

static_assert(offsetof(sophie_hit, model) == SOPH_NFLOATS * sizeof(float), "sophie.h: SOPH_NFLOATS");

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

// the package, placed as the Pi's loader places it (RW data right after the code)
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
  // the state's running floats (each hit's float fields and delay line, the declick): none NaN, inf or denormal
  int bad_floats() const {
    const sophie_state *s = (const sophie_state *)st;
    int bad = 0;
    auto chk = [&](float x) { if (!std::isfinite(x) || (x != 0.0f && std::fpclassify(x) == FP_SUBNORMAL)) ++bad; };
    for (int h = 0; h < SOPH_HITS; ++h) {
      const float *f = (const float *)&s->hit[h];
      for (int i = 0; i < SOPH_NFLOATS; ++i) chk(f[i]);
      for (int i = 0; i < SOPH_MAX_DELAY; ++i) chk(s->hit[h].delay_buf[i]);
    }
    chk(s->declick);
    return bad;
  }
  bool idle() const {
    const sophie_state *s = (const sophie_state *)st;
    for (int h = 0; h < SOPH_HITS; ++h) if (s->hit[h].active) return false;
    return s->declick == 0.0f;
  }
  void params(const float *p16) { p->params(st, p16); }
  void block(float *o) { p->render(st, o, kBlock); }
};

// the manifest's defaults: the factory kick
static const float kDef[16] = {54, 72, 30, 10, 8, 99, 58, 0, 74, 127, 0, 12, 77, 0, 0, 89};

static double rms(const std::vector<float> &x, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < b && i < x.size(); ++i) s += (double)x[i] * x[i];
  return b > a ? std::sqrt(s / (double)(b - a)) : 0.0;
}
static bool finite_in(const std::vector<float> &x) {
  for (float v : x) if (!std::isfinite(v) || v < -1.0f || v > 1.0f) return false;
  return true;
}
static int subnormals(const std::vector<float> &x) {
  int n = 0;
  for (float v : x) n += v != 0.0f && std::fpclassify(v) == FP_SUBNORMAL;
  return n;
}

// one hit (note, velocity 1) on a fresh voice, `secs` long; the state checked after every block
struct Run { std::vector<float> x; int bad = 0; bool guards = true, idle = false; };
static Run hit(const rmp_plugin *p, const float *prm, float note, double secs) {
  Run r;
  Voice v(p);
  v.params(prm);
  p->note_on(v.st, note, 1.0f);
  int total = (int)(secs * kFs) / kBlock * kBlock;
  r.x.resize(total);
  for (int i = 0; i < total; i += kBlock) {
    v.block(&r.x[i]);
    r.bad += v.bad_floats();
  }
  r.guards = v.guards_ok();
  r.idle = v.idle();
  return r;
}

// the common checks of a run that ends in silence
static void check_run(const char *what, const Run &r, double min_rms) {
  double early = rms(r.x, 0, kFs / 10);
  size_t tail0 = r.x.size() - kFs / 5;
  double tail = rms(r.x, tail0, r.x.size());
  CHECK(finite_in(r.x), "%s: not finite or outside +-1", what);
  CHECK(subnormals(r.x) == 0, "%s: %d denormal samples", what, subnormals(r.x));
  CHECK(r.bad == 0, "%s: %d NaN / inf / denormal floats in the state", what, r.bad);
  CHECK(r.guards, "%s: wrote outside its state", what);
  CHECK(early >= min_rms, "%s: rms %.4f in the first 100 ms", what, early);
  CHECK(tail == 0.0 && r.idle, "%s: not silent once decayed (tail %.3g, idle %d)", what, tail, (int)r.idle);
}

// ns per render call: the best mean of five runs of 20000 calls after a warm-up; a hit every 2000 calls (one
// state; `two` a second hit 200 calls after the first, so both ring)
static double time_voice(const rmp_plugin *p, const float *prm, bool two = false) {
  Voice v(p);
  v.params(prm);
  float o[kBlock];
  const int blocks = 20000;
  double mean = 1e30;
  for (int run = 0; run < 6; ++run) {
    double t0 = now_ns();   // the note_ons (a hit clears its delay line) count too
    for (int i = 0; i < blocks; ++i) {
      if (i % 2000 == 0) p->note_on(v.st, 60.0f, 0.8f);
      if (two && i % 2000 == 200) p->note_on(v.st, 60.0f, 0.8f);
      v.block(o);
    }
    if (run) mean = std::min(mean, (now_ns() - t0) / blocks);   // run 0 warms up
  }
  return mean;
}

int main(int argc, char **argv) {
  const char *pkg = argc > 1 ? argv[1] : "../out/sophie.a64.rmp";
  const char *native = argc > 2 ? argv[2] : "../libsophie.dylib";
  rmp_info info;
  const rmp_plugin *p = load(pkg, info);
  CHECK(p, "%s does not load", pkg);
  if (!p) return 1;
  printf("load      %s: id %s, image %u B, bss %u B, state %u B, cpu %u permille, flags 0x%x\n", pkg, info.id,
         info.image_size, info.bss_size, p->state_bytes, p->cpu_permille, info.flags);
  CHECK(strcmp(info.id, "sophie.perc") == 0, "id %s", info.id);
  CHECK(rmp_hf_max_voices(info.flags) == 8 && (info.flags & RMP_HF_INSTANCES), "max_voices: flags 0x%x", info.flags);
  CHECK(rmp_hf_pitch(info.flags) != RMP_PITCH_NONE, "pitch: flags 0x%x", info.flags);
  CHECK(p->state_bytes == sizeof(sophie_state), "state %u, sophie.h's %zu", p->state_bytes, sizeof(sophie_state));
  CHECK(p->state_bytes <= 16384u, "state %u over the Teensy's 16 KB (and the Pi's 64 KB)", p->state_bytes);

  // models: each MODL entry at three notes, 1 s (DEC at the default, 0.28 s)
  static const char *kModel[8] = {"FUSE", "STACK", "SPLIT", "SHARD", "KICK", "KICK ST", "KICK SP", "KICK SH"};
  for (int m = 0; m < 8; ++m) {
    float prm[16];
    memcpy(prm, kDef, sizeof prm);
    prm[1] = (float)(m * 16 + 8);
    for (float note : {36.0f, 60.0f, 96.0f}) {
      Run r = hit(p, prm, note, 1.0);
      char what[48];
      snprintf(what, sizeof what, "model %s note %.0f", kModel[m], note);
      check_run(what, r, 0.02);
      float pk = 0;
      for (float s : r.x) pk = std::max(pk, std::fabs(s));
      if (note == 60.0f) printf("models    %-8s rms %.3f (first 100 ms), peak %.3f\n", kModel[m], rms(r.x, 0, kFs / 10), pk);
    }
  }

  // knobs: each P alone at 0 and 127 (4.5 s: DEC 127 is 4 s), all 0, all 127
  static const char *kShort[16] = {"TUN", "MODL", "COLR", "METL", "FDBK", "SWP", "DEC", "CRSH",
                                   "DRV", "CUT", "RES", "FLT", "RTIM", "RFB", "RMIX", "RTON"};
  int knob_runs = 0;
  for (int k = 0; k < 16; ++k)
    for (float val : {0.0f, 127.0f}) {
      float prm[16];
      memcpy(prm, kDef, sizeof prm);
      prm[k] = val;
      Run r = hit(p, prm, 60.0f, 4.5);
      char what[48];
      snprintf(what, sizeof what, "%s at %.0f", kShort[k], val);
      // CUT 0 is a 40 Hz lowpass over a ~49 Hz kick: quiet, not silent
      check_run(what, r, (k == 9 && val == 0.0f) ? 1e-4 : 0.005);
      ++knob_runs;
    }
  for (float val : {0.0f, 127.0f}) {
    float prm[16];
    for (float &x : prm) x = val;
    Run r = hit(p, prm, 60.0f, 4.5);
    // all 0: CUT 0 lowpass, DEC 0.03 s: barely there; all 127: an 18 kHz-capped shard through a HPF
    check_run(val == 0.0f ? "every P at 0" : "every P at 127", r, 1e-5);
    printf("knobs     every P at %3.0f: rms %.4f (first 100 ms)\n", val, rms(r.x, 0, kFs / 10));
    ++knob_runs;
  }
  printf("knobs     %d runs of one hit: each P alone at 0 and 127, then all at 0, all at 127\n", knob_runs);
  {   // random values each block, a hit every 100 ms at a random note
    Voice v(p);
    float prm[16];
    uint32_t rs = 12345;
    std::vector<float> x(4 * kFs);
    int bad = 0;
    for (int i = 0; i < (int)x.size(); i += kBlock) {
      for (int j = 0; j < 16; ++j) prm[j] = (float)(hrand(&rs) % 128) + (float)(hrand(&rs) % 128) / 128.0f;
      v.params(prm);
      if (i % 4800 == 0) p->note_on(v.st, (float)(hrand(&rs) % 128), (float)(hrand(&rs) % 128) / 127.0f);
      v.block(&x[i]);
      bad += v.bad_floats();
    }
    CHECK(finite_in(x), "random: not finite or outside +-1");
    CHECK(subnormals(x) == 0, "random: denormal samples");
    CHECK(bad == 0, "random: %d bad floats in the state", bad);
    CHECK(v.guards_ok(), "random: wrote outside its state");
    printf("knobs     random each block, a hit every 100 ms: rms %.3f, finite, no denormals\n", rms(x, 0, x.size()));
  }

  // overlap: the crash (Stack, DEC 1.65 s): a second hit at 100 ms leaves the first ringing, byte for byte; a third
  // takes the older one's place, faded out over 2 ms (a smooth 65 Hz Fuse, so a cut would show as a step)
  {
    float crash[16] = {90, 24, 99, 117, 91, 56, 104, 36, 69, 127, 0, 12, 77, 0, 0, 89};
    const int t2 = kFs / 10 / kBlock * kBlock;
    Voice one(p), two(p);
    one.params(crash);
    two.params(crash);
    p->note_on(one.st, 60.0f, 1.0f);
    p->note_on(two.st, 60.0f, 1.0f);
    float o1[kBlock], o2[kBlock];
    bool same = true;
    for (int i = 0; i < kFs / 2; i += kBlock) {
      if (i == t2) p->note_on(two.st, 60.0f, 1.0f);
      one.block(o1);
      two.block(o2);
      if (i < t2) same &= memcmp(o1, o2, sizeof o1) == 0;
    }
    const sophie_state *a = (const sophie_state *)one.st, *b = (const sophie_state *)two.st;
    bool rings = a->hit[0].active && memcmp(&a->hit[0], &b->hit[0], sizeof(sophie_hit)) == 0;
    printf("overlap   2nd hit at 100 ms: the first rings on unchanged under it (%s), the second %s\n",
           rings ? "yes" : "NO", b->hit[1].active ? "playing" : "NOT PLAYING");
    CHECK(same, "the second hit changed what came before it");
    CHECK(rings, "the first hit did not ring on unchanged under the second");
    CHECK(b->hit[1].active, "the second hit is not playing");

    float smooth[16] = {64, 8, 30, 20, 0, 64, 110, 0, 0, 127, 0, 12, 77, 0, 0, 89};
    Voice v(p);
    v.params(smooth);
    std::vector<float> x(kFs / 2);
    const int t3 = 2 * t2 + 37 * kBlock;
    float stolen = 0;
    for (int i = 0; i < (int)x.size(); i += kBlock) {
      if (i == 0 || i == t2) p->note_on(v.st, 60.0f, 1.0f);
      if (i == t3) {
        stolen = ((const sophie_state *)v.st)->hit[0].last * 0.5f;   // SOPH_OUT_GAIN
        p->note_on(v.st, 60.0f, 1.0f);
      }
      v.block(&x[i]);
    }
    float around = 0, before = 0;
    for (int i = t3 - 480; i < t3 - 1; ++i) before = std::max(before, std::fabs(x[i + 1] - x[i]));
    for (int i = t3 - 1; i < t3 + 480; ++i) around = std::max(around, std::fabs(x[i + 1] - x[i]));
    printf("overlap   3rd hit: the oldest's last sample %.3f faded out; largest step %.3f (before it %.3f)\n",
           stolen, around, before);
    CHECK(std::fabs(stolen) > 0.05f, "the test's stolen hit is too quiet to show a cut (%.3f)", stolen);
    CHECK(around < 0.5f * std::fabs(stolen) + 0.1f, "a step of %.3f at the third hit (the stolen hit at %.3f)", around, stolen);
  }

  // cpu: the package (the Pi's code), then the native builds against the starter machines' declared figures
  float heavy[16] = {90, 24, 99, 117, 91, 56, 104, 100, 127, 64, 100, 12, 64, 127, 64, 64};   // crash, CRSH, ring, RES
  double m = time_voice(p, kDef), m2 = time_voice(p, heavy), m3 = time_voice(p, heavy, true);
  printf("cpu       package: %.0f ns mean a 12-frame call (the kick), %.0f (Stack crash, CRSH, ring, RES), %.0f (two of those at once)\n",
         m, m2, m3);
  void *hn = dlopen(native, RTLD_NOW);
  const rmp_plugin *pn = hn ? (const rmp_plugin *)dlsym(hn, "rmp_entry") : nullptr;
  CHECK(pn && pn->init(&g_host) == 0, "%s: no native build", native);
  if (pn) {
    double mn = time_voice(pn, heavy, true);
    std::vector<double> est;
    for (int r = 3; r < argc; ++r) {
      void *h = dlopen(argv[r], RTLD_NOW);
      const rmp_plugin *pr = h ? (const rmp_plugin *)dlsym(h, "rmp_entry") : nullptr;
      if (!pr || pr->init(pr->api == 1u ? &g_host1 : &g_host) != 0) { printf("cpu       (%s: not loaded)\n", argv[r]); continue; }
      float rp[16];
      for (int i = 0; i < 16; ++i) rp[i] = 64.0f;
      rp[1] = 100.0f;
      // the references are held notes: one note_on, then every block
      Voice v(pr);
      v.params(rp);
      pr->note_on(v.st, 60.0f, 0.8f);
      float o[kBlock];
      for (int i = 0; i < 2000; ++i) v.block(o);
      double mr = 1e30;
      for (int run = 0; run < 5; ++run) {
        double t0 = now_ns();
        for (int i = 0; i < 20000; ++i) v.block(o);
        mr = std::min(mr, (now_ns() - t0) / 20000);
      }
      est.push_back(pr->cpu_permille * mn / mr);
      printf("cpu       native: sophie %.0f ns (two hits), %s %.0f ns (declared %u): ~%.0f permille\n", mn,
             strrchr(argv[r], '/') + 1, mr, pr->cpu_permille, est.back());
    }
    if (!est.empty()) {
      std::sort(est.begin(), est.end());
      double e = est[est.size() / 2];
      printf("cpu       estimate %.0f permille (median, two hits ringing), declared %u\n", e, p->cpu_permille);
      CHECK(e <= 2.0 * p->cpu_permille, "the estimate %.0f is over the guard's 2x of the declared %u", e, p->cpu_permille);
    }
  }
  printf(g_fail ? "sophie_check: %d FAILED\n" : "sophie_check: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
