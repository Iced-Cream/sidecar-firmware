// core/tests/test_main_out.cpp - core::MainOut (the voices on MAIN L/R for a stock Rytm): the sum at kGain, the
// fade-in from 0 over 200 ms after Reset, the -12 dBFS limit both ways, Next16 the same in int16, the peak.
#include <cmath>

#include "check.h"
#include "main_out.h"

using core::MainOut;

int main() {
  MainOut m;
  int16_t v[8] = {0};
  v[0] = 8192;                                    // voice 0 at 0.25, voice 3 at 0.125
  v[3] = 4096;
  CHECK(m.Fading() && m.Next(v) == 0.0f);         // the fade starts at 0
  float x1 = m.Next(v);
  CHECK(x1 > 0.0f && x1 < 0.001f);
  for (uint32_t i = 2; i < MainOut::kFadeFrames; ++i) m.Next(v);
  CHECK(!m.Fading());
  CHECK(std::fabs(m.Next(v) - 0.375f * MainOut::kGain) < 1e-6f);   // the sum at kGain, faded in
  CHECK(m.Next16(v) == (int16_t)(0.375f * MainOut::kGain * 32768.0f));
  CHECK(std::fabs(m.TakePeak() - 0.1875f) < 1e-6f && m.TakePeak() == 0.0f);
  for (int i = 0; i < 8; ++i) v[i] = 32767;       // all full scale: held at -12 dBFS
  CHECK(m.Next(v) == MainOut::kLimit && m.Next16(v) == 8192);
  for (int i = 0; i < 8; ++i) v[i] = -32768;
  CHECK(m.Next(v) == -MainOut::kLimit && m.Next16(v) == -8192);
  CHECK(m.TakePeak() == MainOut::kLimit);
  m.Reset();                                      // a new stream: from silence again
  CHECK(m.Fading() && m.Next(v) == 0.0f);
  float g = m.Gain();                             // the two halves = Next
  CHECK(g > 0.0f && std::fabs(m.Sum(v, g) + 8.0f * MainOut::kGain * g) < 1e-6f);   // 8 x -1.0 x kGain x g
  printf("test_main_out: PASS\n");
  return 0;
}
