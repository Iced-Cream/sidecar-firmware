// pi/engine/a4_in.cpp - see a4_in.h
#include "a4_in.h"

#include <math.h>

#include "device/a4.h"
#include "drift_src.h"
#include "fx/out.h"
#include "ob_block.h"

namespace a4in {

static DriftSrc s_Src;
static float s_fGain = 0.5f;
static unsigned s_nChannel = core::device::kA4Main;

void Init(unsigned left_channel, unsigned att_db) {
  s_Src.init();
  s_nChannel = left_channel + 1 < (unsigned)core::device::kA4InChannels ? left_channel : core::device::kA4Main;
  s_fGain = powf(10.0f, -(float)att_db / 20.0f);
}

void In(const uint8_t* block) {
  for (int f = 0; f < ob::kFramesPerBlock; ++f)
    s_Src.push((float)ob::block_sample(block, core::device::kA4InChannels, f, (int)s_nChannel) / 2147483648.0f,
               (float)ob::block_sample(block, core::device::kA4InChannels, f, (int)s_nChannel + 1) / 2147483648.0f);
}

void Mix(int32_t (*out)[12], unsigned n) {
  float o[ob::kFramesPerBlock][2];
  if (n > (unsigned)ob::kFramesPerBlock) n = ob::kFramesPerBlock;
  s_Src.pull(o, n);
  const uint8_t* pre = core::out::device().pre;
  for (unsigned f = 0; f < n; ++f)
    for (unsigned c = 0; c < 2; ++c) {
      int64_t s = (int64_t)out[f][pre[c]] + (int64_t)(o[f][c] * s_fGain * 2147483648.0f);
      out[f][pre[c]] = s > 2147483647 ? 2147483647 : s < -2147483647 - 1 ? -2147483647 - 1 : (int32_t)s;
    }
}

Stats Read() { return {(int)s_Src.fill(), (int)s_Src.ppm(), s_Src.under(), s_Src.low(), s_Src.high(), s_Src.full()}; }

}  // namespace a4in
