// core/tests/test_voice_levels.cpp - the voices' levels: LEV 0 is silence on DAISYSP and PLAITS (no 0.05 floor), PLAITS'
// LEV and velocity scale its output (an output gain: Plaits' own decay envelope stays), and every engine at its default
// knobs (64, LEV 100), full velocity, peaks under the output stage's -12 dBFS ceiling (core::MainOut) once kVoiceGain
// (voices.h) is applied, the knee taking off less than 1 dB from the loudest.
#include <cmath>
#include <cstdio>
#include <vector>

#include "check.h"
#include "main_out.h"
#include "voices.h"

using namespace core;

static std::vector<uint8_t> report(int vb, int note, int vel, std::vector<float> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x04, (uint8_t)vb, (uint8_t)note, (uint8_t)vel};
  p.resize(16, 0);
  for (float x : p) { int i = (int)x; m.push_back(i); m.push_back((int)((x - i) * 128) & 0x7f); }
  m.push_back(0xf7);
  return m;
}

static int16_t ring[kBlock * 64][kVoices];
// one trig on voice 2, blocks rendered; the voice row's peak from block `from` on (int16 counts)
static int peak_of(int vb, int vel, const std::vector<float>& p, int blocks, int from = 0) {
  auto t = report(vb | 2, 48, vel, p);
  voice_report(0x04, t.data(), (int)t.size());
  uint64_t mach[kMachines] = {0};
  int pk = 0;
  for (int b = 0; b < blocks; ++b) {
    render_voices(ring, kBlock * 64, (b % 64) * kBlock, 0, kVoices, mach);
    if (b < from) continue;
    for (int s = 0; s < kBlock; ++s) {
      int a = std::abs((int)ring[(b % 64) * kBlock + s][2]);
      pk = a > pk ? a : pk;
    }
  }
  voice_silence(2);
  for (int b = 0; b < 400; ++b) render_voices(ring, kBlock * 64, (b % 64) * kBlock, 0, kVoices, mach);
  return pk;
}
static double db(int a, int b) { return 20 * std::log10((double)a / b); }

int main() {
  hooks.to_rytm = [](const uint8_t*, int) {};
  voices_init();
  const int kSec = 4000;   // blocks in a second
  // DAISYSP STRING (ENG zone 1 of 3), vel 127. DMP 0 (the longest ring): LEV 0 silent after the attack (< -90 dBFS;
  // the old 0.05 floor left it at -27 dBFS, still ringing). DMP 64: LEV 32 6 to 12 dB under LEV 64 (STRING's strike
  // scales by the level twice, its string once: daisy_engine.h)
  {
    auto P = [](float lev, float dmp) { return std::vector<float>{64, 64, 64, dmp, 0, 64, 64, lev, 64, 64, 0, 0, 0, 0, 0, 0}; };
    int z = peak_of(kDaisy, 127, P(0, 0), kSec, 200), hi = peak_of(kDaisy, 127, P(64, 64), kSec), lo = peak_of(kDaisy, 127, P(32, 64), kSec);
    printf("DAISYSP STRING: LEV 0 %d; DMP 64: LEV 64 %d, 32 %d\n", z, hi, lo);
    CHECK(z < 1);   // -90 dBFS is 1.04 counts
    CHECK(db(lo, hi) < -6.0 && db(lo, hi) > -12.0);
  }
  // PLAITS VA (ENG zone 5 of 17), VOL 100: LEV 127 against 64 by the levels' ratio, VEL 127 velocity 20 against 127
  // too; LEV 0 silent
  {
    auto P = [](float lev, float sens) {
      return std::vector<float>{(5 * 128 + 64) / 17.0f, 64, 64, 64, 64, 64, 64, lev, 64, 64, 0, 0, 0, sens, 0, 100};
    };
    int hi = peak_of(kPlaits, 127, P(127, 120), kSec / 2), mid = peak_of(kPlaits, 127, P(64, 120), kSec / 2);
    int v127 = peak_of(kPlaits, 127, P(127, 127), kSec / 2), v20 = peak_of(kPlaits, 20, P(127, 127), kSec / 2);
    int z = peak_of(kPlaits, 127, P(0, 120), kSec / 2, 200);
    printf("PLAITS VA: LEV 127 %d, 64 %d, 0 %d; VEL 127: vel 127 %d, vel 20 %d\n", hi, mid, z, v127, v20);
    CHECK(std::fabs(db(mid, hi) - 20 * std::log10(64.0 / 127)) < 1.0);
    CHECK(std::fabs(db(v20, v127) - 20 * std::log10(20.0 / 127)) < 1.0);
    CHECK(z < 1);
  }
  // every engine, default knobs, vel 127: the voice row's peak under the ceiling (kLimit, -12 dBFS; before kVoiceGain
  // they reached -4.4 dBFS and the old clamp clipped them), and the soft knee (from kKnee x kLimit, -15.1 dBFS) takes
  // less than 1 dB off any of them. With kVoiceGain 0.35 DAISYSP STRING (-13.5 dBFS) and PLAITS MODAL (-14.3) are in
  // the knee, the rest under it (sample-exact).
  {
    const int knee = (int)(MainOut::kKnee * MainOut::kLimit * 32768.0f), ceil = (int)(MainOut::kLimit * 32768.0f);
    int worst = 0, in_knee = 0;
    auto at = [&](int vb, std::vector<float> p, int blocks) {
      int pk = peak_of(vb, 127, p, blocks);
      float x = pk / 32768.0f;
      CHECK(pk < ceil && MainOut::Limit(x) > 0.891f * x);   // under -12 dBFS; the knee's cut under 1 dB
      in_knee += pk > knee;
      worst = pk > worst ? pk : worst;
    };
    for (int z = 0; z < kPlaitsEngs; ++z)
      at(kPlaits, {(z * 128 + 64) / 17.0f, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100}, kSec / 2);
    for (int m = 0; m < 3; ++m) at(kDaisy, {(m * 128 + 64) / 3.0f, 64, 64, 64, 0, 64, 64, 100, 64, 64, 0, 0, 0, 0, 0, 0}, kSec);
    for (int pat = 0; pat < 128; pat += 16) at(kJuno, {(float)pat, 64, 64, 64, 64, 64, 64, 100, 64, 64, 64, 64, 64, 64, 64, 64}, kSec / 2);
    for (int pat = 0; pat < 32; pat += 4) at(kFm, {(float)pat, 64, 64, 64, 64, 64, 64, 100, 64, 64, 64, 64, 64, 64, 64, 64}, kSec / 2);
    for (int e = 0; e < 128; e += 16) at(kMd, {(float)e, 64, 64, 64, 64, 64, 64, 100, 64, 64, 64, 64, 64, 64, 64, 64}, kSec / 2);
    printf("loudest engine at its defaults: %d (%.1f dBFS), the knee %d, the ceiling %d; %d in the knee\n", worst,
           20 * std::log10(worst / 32768.0), knee, ceil, in_knee);
    CHECK(in_knee <= 2);   // (STRING and MODAL: a hotter default would show here)
  }
  printf("test_voice_levels: PASS\n");
  return 0;
}
