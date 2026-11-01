// pi/engine/tests/bench_render.cpp - the render paths' cost off the Pi (docs/superpowers/specs/2026-10-31-neon-pass.md):
// the boot bench's machines (bridge::bench), the INSERT pairs, the send FX, the output stage, each with an FNV hash of
// its output, so two builds can be compared bit for bit. Built by core/tests/bench/bench.mk: natively with the Mac's clang
// or, in the rytm-circle image, with GCC and the Pi's flags (bench.sh). Not a test: no pass or fail.
//   bench_render [-n REPEATS] [-p PROFILE.txt] [-m NAME]   -p: a 1 kHz PC sample per bench (Linux AArch64): its address histogram
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <string>

#include "../bridge.h"
#include "../output.h"
#include "fx/insert.h"
#include "fx/send_fx.h"
#include "platform.h"

namespace song_pi { void Tick() {} }

static double now_s() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

#if defined(__linux__) && defined(__aarch64__)
#include <ucontext.h>
static const int kMaxPc = 1 << 21;
static volatile uint64_t* g_pcs;
static volatile int g_npc, g_prof;
static void on_prof(int, siginfo_t*, void* u) {
  if (g_prof && g_npc < kMaxPc) g_pcs[g_npc++] = ((ucontext_t*)u)->uc_mcontext.pc;
}
static void prof_init() {
  g_pcs = (uint64_t*)calloc(kMaxPc, 8);
  struct sigaction a;
  memset(&a, 0, sizeof a);
  a.sa_sigaction = on_prof, a.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGPROF, &a, nullptr);
  itimerval t = {{0, 1000}, {0, 1000}};
  setitimer(ITIMER_PROF, &t, nullptr);
}
#else
static void prof_init() {}
static volatile uint64_t* g_pcs;
static volatile int g_npc, g_prof;
#endif

static FILE* g_prof_out;
static const char* g_only;
static int g_repeats = 3;

// the best of g_repeats runs of f (seconds of CPU for 1 s of audio); hash the runs' last
template <class F>
static void run(const char* name, F f) {
  if (g_only && !strstr(name, g_only)) return;
  double best = 1e9;
  uint32_t h = 0;
  int n0 = g_npc;
  g_prof = 1;
  for (int r = 0; r < g_repeats; ++r) {
    double t = f(&h);
    if (t < best) best = t;
  }
  g_prof = 0;
  if (g_prof_out) {
    fprintf(g_prof_out, "# %s\n", name);
    std::map<uint64_t, int> m;
    for (int i = n0; i < g_npc; ++i) ++m[(uint64_t)g_pcs[i]];
    for (auto& k : m) fprintf(g_prof_out, "%llx %d\n", (unsigned long long)k.first, k.second);
    g_npc = n0;
  }
  printf("%-22s %8.3f %%  %08x\n", name, best * 100.0, h);   // % of real time, one core of this machine
  fflush(stdout);
}

static void bench_sendfx(int type) {
  using namespace core::fx;
  static SendFx* fx = nullptr;
  if (!fx) fx = new SendFx(48000.0f);
  char nm[32];
  snprintf(nm, sizeof nm, "sendfx %d", type);
  run(nm, [&](uint32_t* h) {
    fx->Reset();
    static Control c;
    memset((void*)&c, 0, sizeof c);
    c.knob[0][0] = (uint8_t)type;
    for (int k = 1; k < kKnobs; ++k) c.knob[0][k] = (uint8_t)(k == 1 ? 0 : 64);
    for (int t = 0; t < kTracks; ++t) c.send[t][0] = 100;
    static float st[96][8], o[96][2], m[96][2];
    double tot = 0;
    uint32_t hh = 2166136261u;
    for (int b = 0; b < 500; ++b) {
      for (int i = 0; i < 96; ++i)
        for (int v = 0; v < 8; ++v) st[i][v] = 0.2f * sinf(0.01f * (float)(b * 96 + i) * (float)(v + 1));
      double t0 = now_s();
      fx->Process(st, 96, o, c, m);
      tot += now_s() - t0;
      for (int i = 0; i < 96; ++i) hh = (hh ^ (uint32_t)(int32_t)((o[i][0] + m[i][1]) * 1048576.0f)) * 16777619u;
    }
    *h = hh;
    return tot;
  });
}

static void bench_output(const char* name, int mode, bool pre) {
  run(name, [&](uint32_t* h) {
    static Output* o = nullptr;
    if (!o) o = new Output();
    static core::out::Control c;
    memset((void*)&c, 0, sizeof c);
    for (int v = 0; v < 8; ++v) {
      c.route[v] = pre ? core::out::kPre : core::out::kTrk, c.lev[v] = 100, c.pan[v] = (uint8_t)(32 + 8 * v), c.wid[v] = 64, c.open[v] = 1;
    }
    o->SetOutControl(&c);
    o->SetMode(mode ? Output::kTracks : Output::kMain);
    static int16_t blk[12][8], blkr[12][8];
    static int32_t out[12][Output::kChannels];
    double tot = 0;
    uint32_t hh = 2166136261u;
    for (int b = 0; b < 4000; ++b) {
      for (int i = 0; i < 12; ++i)
        for (int v = 0; v < 8; ++v) blk[i][v] = (int16_t)(12000.0f * sinf(0.01f * (float)(b * 12 + i) * (float)(v + 1))), blkr[i][v] = blk[i][v] / 2;
      o->Push(blk, 12, blkr, 0xff);
      double t0 = now_s();
      o->Pull(out, 12);
      tot += now_s() - t0;
      for (int i = 0; i < 12; ++i)
        for (int k = 0; k < Output::kChannels; ++k) hh = (hh ^ (uint32_t)out[i][k]) * 16777619u;
    }
    *h = hh;
    return tot;
  });
}

int main(int argc, char** argv) {
  const char* prof = nullptr;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "-n")) g_repeats = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "-p")) prof = argv[i + 1];
    if (!strcmp(argv[i], "-m")) g_only = argv[i + 1];
  }
  char tmpl[] = "/tmp/pi-bench-XXXXXX";
  if (!mkdtemp(tmpl)) return 1;
  core::host_set_card(tmpl);
  bridge::selftest_content();
  if (getenv("BENCH_SILENT")) {   // the sample all zeros: a playing voice whose tanhf takes its fast path (a quiet sample on the Pi)
    FILE* f = fopen(core::host_path("/Samples/pi-test/sine440.wav").c_str(), "r+b");
    if (f) {
      static char z[96000 * 2];
      fseek(f, 44, SEEK_SET), fwrite(z, sizeof z, 1, f), fclose(f);
    }
  }
  bridge::begin([](const uint8_t*, int) {});
  for (int i = 0; i < 20000; ++i) bridge::idle_prepare(), bridge::idle_save(), bridge::idle_stream();   // the loads the boot queued
  if (prof) g_prof_out = fopen(prof, "w"), prof_init();
  static const char* const kM[] = {"plaits", "juno", "fm", "daisy", "dsps", "wt", "gr", "livein", "chop", "md"};
  for (const char* m : kM)
    run(m, [&](uint32_t* h) {
      uint32_t w = 0;
      int pk = 0;
      uint64_t t = bridge::bench(m, &w, &pk, h);
      return t / (double)core::cycles_per_second();
    });
  static const int kP[][2] = {{1, 6}, {5, 4}, {3, 7}, {2, 8}, {0, 0}};
  for (auto& p : kP) {
    char nm[48];
    snprintf(nm, sizeof nm, "ins %s+%s", bridge::ins_bench_name(p[0]), bridge::ins_bench_name(p[1]));
    run(nm, [&](uint32_t* h) {
      uint32_t w = 0;
      return bridge::ins_bench(p[0], p[1], &w, h) / (double)core::cycles_per_second();
    });
  }
  run("amp", [&](uint32_t* h) {
    uint32_t w = 0;
    *h = 0;
    return bridge::amp_bench(&w) / (double)core::cycles_per_second();
  });
  static const int kT[] = {core::fx::kChorus, core::fx::kComb, core::fx::kTape, core::fx::kRings, core::fx::kCloudSeed, core::fx::kCrush, core::fx::kRingMod, core::fx::kHalo};
  for (int t : kT) bench_sendfx(t);
  bench_output("out main trk", 0, false);
  bench_output("out tracks trk", 1, false);
  bench_output("out main pre", 0, true);
  {
    if (!g_only)
      for (unsigned r = 0;; ++r) {   // the best of the repeats (a host's cores differ: P and E)
        uint64_t c = ~0ull, n = ~0ull, c1, n1;
        const char* nm = nullptr;
        for (int k = 0; k < g_repeats; ++k) {
          nm = bridge::neon_bench(r, &c1, &n1);
          if (!nm) break;
          c = c1 < c ? c1 : c, n = n1 < n ? n1 : n;
        }
        if (!nm) break;
        printf("neon %-14s C %8.1f us, NEON %8.1f us (%3d %%)\n", nm, c / 1000.0, n / 1000.0, c ? (int)(n * 100 / c) : 0);
      }
  }
  if (g_prof_out) fclose(g_prof_out);
  return 0;
}
