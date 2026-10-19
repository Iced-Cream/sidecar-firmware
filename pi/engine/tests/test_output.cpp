// pi/engine/tests/test_output.cpp - the output stage: main mode sums onto MAIN L/R, tracks mode puts each voice on its
// Overbridge channel, the fade-in over 200 ms, the -12 dBFS limit, silence (counted) when the FIFO runs dry.
#include <cmath>

#include "check.h"
#include "output.h"

static int16_t blk[12][8];

int main() {
  static Output o;
  int32_t f[7][12];
  o.Pull(f, 7);   // empty: silence, counted
  CHECK(o.Underflows() == 7 && f[3][0] == 0);
  for (int s = 0; s < 12; ++s) for (int v = 0; v < 8; ++v) blk[s][v] = (v == 2) ? 8192 : 0;   // voice 2 at 0.25
  for (int k = 0; k < 2000; ++k) CHECK(o.Push(blk, 12) == 12 || o.Level() > 2000);
  CHECK(o.Level() == Output::kFifo);
  o.Reset();                                                  // a new stream: the old frames are dropped
  CHECK(o.Level() == 0);
  o.Push(blk, 12);
  o.Pull(f, 7);
  CHECK(f[0][0] == 0 && f[1][0] > 0 && f[1][0] < 2000000);   // the fade starts at 0
  for (int k = 0; k < 1400; ++k) {                            // past 200 ms, the FIFO never empty
    o.Push(blk, 12);
    o.Pull(f, 7);
  }
  float main_l = f[6][0] / 2147483648.0f;
  CHECK(std::fabs(main_l - 0.25f * Output::kMainGain) < 1e-4f && f[6][0] == f[6][1]);
  CHECK(f[6][Output::kVoiceToChannel[2]] == 0);                // main mode: the voice channels silent
  o.SetMode(Output::kTracks);
  o.Pull(f, 7);
  CHECK(f[0][0] == 0 && f[0][1] == 0);
  CHECK(std::fabs(f[0][Output::kVoiceToChannel[2]] / 2147483648.0f - 0.25f) < 1e-4f);
  for (int s = 0; s < 12; ++s) blk[s][2] = 32767;                // full scale: limited to -12 dBFS
  while (o.Level()) o.Pull(f, 7);
  o.Push(blk, 12);
  o.Pull(f, 7);
  CHECK(f[0][Output::kVoiceToChannel[2]] == (int32_t)(Output::kLimit * 2147483648.0f));
  CHECK(o.TakePeak() > 0.24f && o.TakePeak() == 0.0f);
  const uint8_t map[8] = {4, 7, 5, 8, 10, 9, 11, 6};             // ob_block.h's kVoiceToRytm
  for (int v = 0; v < 8; ++v) CHECK(Output::kVoiceToChannel[v] == map[v]);
  printf("test_output: PASS\n");
  return 0;
}
