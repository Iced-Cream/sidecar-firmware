// core/tests/mc/test_grain_cores.cpp - DSP SAMPLE's GRAIN mode (sim/sampler/sample_modes.h) as the Pi runs it: two
// render cores at once (cores 0 and 1 here, the Pi's 1 and 2) and a control core (2 here, the Pi's 3) that
// triggers and stops the voices between blocks (core/voices.cpp: voice_report, ram_release). Six GRAIN voices a
// render core, dense enough that each core's grains hit kGrainBudget, retriggered by the control core every 200
// blocks while they sound. Against one thread doing the same in turn with the same core indices: bit-identical;
// every core's count reaches the budget, is back at 0 after the control core's last Stop, and the voices still
// sound in the last 200 blocks. Two bugs it catches: one shared count (the render cores' ++ and -- raced), and a
// grain's count given back to the caller's core (the control core's Stop leaked the render core's grains: after
// a few retriggers the render core sat at the budget, GRAIN silent until a reboot).
// Built with RYTM_RENDER_CORES=3 (core/Makefile's build/mc).
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "../check.h"
#include "sample_modes.h"

static thread_local int t_core = 0;
extern "C" int rytm_render_core() { return t_core; }

constexpr int kPerCore = 6, kVoicesAll = 2 * kPerCore, kN = 12, kBlocks = 2000, kFrames = 96000, kControl = 2;
static int16_t pcm[kFrames + 1];
// SD SAMPLE: TUN FIN SMP MOD (56: zone 3, GRAIN) STA END (127: 0.5 s grains) LOP (127: 256 a second) LEV
static const float kSp[8] = {64, 64, 0, 56, 60, 127, 127, 100};

struct Result {
  int max_count[2] = {0, 0}, end_count[3] = {0, 0, 0};
};

static void run(bool two, std::vector<float>& out, Result& r) {
  static rytm::SampleModes sm[kVoicesAll];
  t_core = kControl;
  for (int i = 0; i < kVoicesAll; ++i) {
    sm[i].Init(i + 1);
    sm[i].Set(pcm, kFrames, 48000.0f);
  }
  auto render = [&](int c, int b) {   // one block of core c's voices, on core c
    t_core = c;
    for (int i = c * kPerCore; i < (c + 1) * kPerCore; ++i)
      sm[i].Render(kSp, &out[((size_t)i * kBlocks + b) * kN], kN);
    int n = rytm::SampleModes::ActiveGrains();
    if (n > r.max_count[c]) r.max_count[c] = n;
  };
  auto control = [&](int b) {   // between blocks, on the control core: the trigs
    t_core = kControl;
    if (b % 200 == 0)
      for (int i = 0; i < kVoicesAll; ++i) sm[i].Trigger(48.0f + i, kSp);   // the sounding grains stop
  };
  if (two) {
    std::atomic<int> gen{0}, done{0};
    auto worker = [&](int c) {
      for (int b = 0; b < kBlocks; ++b) {
        while (gen.load(std::memory_order_acquire) != b + 1) {}
        render(c, b);
        done.fetch_add(1, std::memory_order_acq_rel);
      }
    };
    std::thread a(worker, 0), w(worker, 1);
    for (int b = 0; b < kBlocks; ++b) {
      control(b);
      gen.store(b + 1, std::memory_order_release);
      while (done.load(std::memory_order_acquire) != 2 * (b + 1)) {}
    }
    a.join();
    w.join();
  } else {
    for (int b = 0; b < kBlocks; ++b) {
      control(b);
      for (int c = 0; c < 2; ++c) render(c, b);
    }
  }
  t_core = kControl;
  for (int i = 0; i < kVoicesAll; ++i) sm[i].Stop();   // the control core stops them all
  for (int c = 0; c < 3; ++c) {   // each core's count, as that core sees it
    t_core = c;
    r.end_count[c] = rytm::SampleModes::ActiveGrains();
  }
  t_core = kControl;
}

static double rms(const std::vector<float>& x, int voice, int b0, int b1) {
  double s = 0;
  for (size_t k = ((size_t)voice * kBlocks + b0) * kN; k < ((size_t)voice * kBlocks + b1) * kN; ++k) s += x[k] * x[k];
  return std::sqrt(s / ((b1 - b0) * kN));
}

int main() {
  for (int i = 0; i <= kFrames; ++i) pcm[i] = (int16_t)(12000 * sinf(6.2831853f * 220.0f * (i % kFrames) / 48000.0f));
  std::vector<float> ref((size_t)kVoicesAll * kBlocks * kN), two(ref.size());
  for (int pass = 0; pass < 2; ++pass) {
    Result r;
    std::vector<float>& x = pass ? two : ref;
    run(pass == 1, x, r);
    const char* what = pass ? "two threads" : "one thread";
    if (r.end_count[0] || r.end_count[1] || r.end_count[2])
      fprintf(stderr, "%s: grain counts after the last Stop: %d %d %d (control core %d)\n", what, r.end_count[0],
              r.end_count[1], r.end_count[2], kControl);
    CHECK(r.end_count[0] == 0 && r.end_count[1] == 0 && r.end_count[2] == 0);
    CHECK(r.max_count[0] == rytm::SampleModes::kGrainBudget && r.max_count[1] == rytm::SampleModes::kGrainBudget);
    for (int c = 0; c < 2; ++c) {   // each core's first voice sounds, at the start and after 9 retriggers
      if (rms(x, c * kPerCore, kBlocks - 200, kBlocks) <= 0.001)
        fprintf(stderr, "%s: core %d's voice silent in the last 200 blocks\n", what, c);
      CHECK(rms(x, c * kPerCore, 0, 200) > 0.001 && rms(x, c * kPerCore, kBlocks - 200, kBlocks) > 0.001);
    }
  }
  for (int i = 0; i < kVoicesAll; ++i)
    CHECK(memcmp(&ref[(size_t)i * kBlocks * kN], &two[(size_t)i * kBlocks * kN], sizeof(float) * kBlocks * kN) == 0);
  printf("test_grain_cores: PASS (%d GRAIN voices on two render threads at the budget, retriggered and stopped "
         "from a control thread: bit-identical to one, counts 0, still sounding)\n", kVoicesAll);
  return 0;
}
