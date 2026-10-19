// core/tests/test_voices.cpp - the voices on the Mac: a PLAITS trig sounds on its voice only, a JUNO note held
// until 0x11 then released, DIGIDRUM and DAISYSP sound, a user slot without plugins is silent, the machine stats
// count the block. Reports as sim/protocol.py builds them (core_golden.py's helpers in C++).
#include <cmath>
#include <cstring>
#include <vector>

#include "check.h"
#include "voices.h"

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }

static std::vector<uint8_t> report(int cmd, int vb, int note, int vel, std::vector<float> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vb};
  if (cmd == 0x04) { m.push_back(note); m.push_back(vel); }
  p.resize(16, 0);
  for (float x : p) { int i = (int)x; m.push_back(i); m.push_back((int)((x - i) * 128) & 0x7f); }
  m.push_back(0xf7);
  return m;
}

static int16_t ring[core::kBlock * 400][core::kVoices];

static double rms(int voice, int frames) {   // frames from ring row 0
  double s = 0;
  for (int i = 0; i < frames; ++i) s += (double)ring[i][voice] * ring[i][voice];
  return std::sqrt(s / frames);
}

static void render(int blocks) {
  uint64_t mach[core::kMachines] = {0};
  for (int b = 0; b < blocks; ++b) core::render_voices(ring, core::kBlock * 400, b * core::kBlock, 0, core::kVoices, mach);
}

int main() {
  using namespace core;
  hooks.to_rytm = out;
  voices_init();
  render(10);
  for (int v0 = 0; v0 < kVoices; ++v0) CHECK(rms(v0, 120) == 0);   // nothing live: silence
  auto t = report(0x04, kPlaits | 0, 48, 100, {124, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100});
  voice_report(0x04, t.data(), (int)t.size());
  CHECK(v[0].live && !v[0].juno && v[0].patch.engine == kPlaitsModels[16]);
  render(400);
  CHECK(rms(0, 4800) > 300 && rms(1, 4800) == 0);
  auto j = report(0x04, kJuno | 1, 60, 100, {0, 80, 20, 64, 10, 64, 64, 127});
  voice_report(0x04, j.data(), (int)j.size());
  CHECK(v[1].juno && juno_pat[1] == 0);
  CHECK(!g_out.empty() && g_out.back()[4] == 0x15 && g_out.back()[5] == 1);   // the patch's knobs to the Rytm
  render(400);
  double held = rms(1, 4800);
  CHECK(held > 300);
  voice_note_off(1);
  for (int k = 0; k < 50; ++k) render(400);   // 5 s: any factory patch's release is over by then
  CHECK(rms(1, 4800) < held / 4);   // released
  auto d = report(0x04, kDigidrum | 4, 60, 100, {6, 64, 64, 64, 64, 64, 64, 64});
  voice_report(0x04, d.data(), (int)d.size());
  auto s = report(0x04, kDaisy | 3, 57, 100, {0, 64, 64, 64, 0, 64, 64, 100});
  voice_report(0x04, s.data(), (int)s.size());
  auto u = report(0x04, 0x50 | 6, 60, 100, {64});
  voice_report(0x04, u.data(), (int)u.size());
  CHECK(v[6].user == 0 && !v[6].live);   // no plugins: silent
  render(400);
  CHECK(rms(4, 4800) > 100 && rms(3, 4800) > 100 && rms(6, 4800) == 0);
  uint64_t mach[kMachines] = {0};
  // 100 blocks: host cycles() are nanoseconds (core/host/platform_host.cpp), but macOS's CLOCK_MONOTONIC steps
  // in whole microseconds, so one block's voice may read 0
  uint32_t dt = 0;
  for (int b = 0; b < 100; ++b) dt += render_voices(ring, kBlock * 400, b * kBlock, 0, kVoices, mach);
  CHECK(dt > 0 && mach[kMPlaits] > 0 && mach[kMDd] > 0 && mach[kMDaisy] > 0 && mach[kMUser] == 0);
  printf("test_voices: PASS\n");
  return 0;
}
