// sim/plaits/operator_neon.h - the 6-OP FM's operators 4 samples wide (AArch64: the Pi, the Mac; the Teensy keeps the C).
// teensy/setup_plaits_lib.sh includes this at the end of plaits/dsp/fm/operator.h, inside namespace plaits::fm, and
// renames Plaits' RenderOperators to RenderOperatorsC. An operator modulated from outside (or not at all) depends on no
// earlier sample of its own, so its samples can run side by side; a run of n operators without feedback becomes n passes
// over the block. Feedback (each sample needs the one before): Plaits' C. The same arithmetic as SinePM lane by lane, the
// amplitude's ramp summed sample by sample as the C does: bit-exact (core/tests/test_neon.cpp). (<arm_neon.h>: the same
// patch, at the top of operator.h.)
#if defined(__ARM_NEON) && defined(__aarch64__) && !defined(CORE_NO_NEON)
#define RYTM_FM_NEON 1

// One operator's block. mod (null: none) x mod_a (null: 1) is its modulation, a pair because the C's multiply fuses with
// SinePM's offset. out_a null: out = the output (additive: added); else out, out_a the next operator's mod, mod_a.
inline void RenderOneNeon(Operator* op, uint32_t frequency, float increment, const float* mod, const float* mod_a,
                          float* out, float* out_a, size_t size, bool additive) {
  uint32_t phase = op->phase;
  float amplitude = op->amplitude;
  const float kScale = 4294967296.0f / 64.0f;   // SinePM's: max_uint32 / (2 * max_index)
  const float32x4_t fix = vdupq_n_f32(1.0f / 4294967296.0f), offset = vdupq_n_f32(32.0f), scale = vdupq_n_f32(kScale);
  uint32_t p[4] = {phase + frequency, phase + 2 * frequency, phase + 3 * frequency, phase + 4 * frequency};
  uint32x4_t ph = vld1q_u32(p);
  const uint32x4_t step = vdupq_n_u32(4 * frequency);
  size_t s = 0;
  for (; s + 4 <= size; s += 4) {
    float32x4_t am = vdupq_n_f32(amplitude);   // (the ramp in registers: four stores and a q load would stall an A72)
    amplitude += increment, am = vsetq_lane_f32(amplitude, am, 1);
    amplitude += increment, am = vsetq_lane_f32(amplitude, am, 2);
    amplitude += increment, am = vsetq_lane_f32(amplitude, am, 3);
    amplitude += increment;
    uint32x4_t q = ph;
    if (mod) {
      const float32x4_t m = vld1q_f32(mod + s);
      const float32x4_t pm = mod_a ? vfmaq_f32(offset, m, vld1q_f32(mod_a + s)) : vaddq_f32(m, offset);
      q = vaddq_u32(q, vshlq_n_u32(vcvtq_u32_f32(vmulq_f32(pm, scale)), 6));
    }
    ph = vaddq_u32(ph, step);
    const uint32x4_t idx = vshrq_n_u32(q, 32 - kSineLUTBits);
    const float32x4_t fr = vmulq_f32(vcvtq_f32_u32(vshlq_n_u32(q, kSineLUTBits)), fix);
    const float32x4_t lo = vcombine_f32(vld1_f32(lut_sine + vgetq_lane_u32(idx, 0)), vld1_f32(lut_sine + vgetq_lane_u32(idx, 1)));
    const float32x4_t hi = vcombine_f32(vld1_f32(lut_sine + vgetq_lane_u32(idx, 2)), vld1_f32(lut_sine + vgetq_lane_u32(idx, 3)));
    const float32x4_t a = vuzp1q_f32(lo, hi), b = vuzp2q_f32(lo, hi);
    const float32x4_t y = vfmaq_f32(a, vsubq_f32(b, a), fr);
    if (out_a) vst1q_f32(out + s, y), vst1q_f32(out_a + s, am);
    else if (additive) vst1q_f32(out + s, vfmaq_f32(vld1q_f32(out + s), y, am));
    else vst1q_f32(out + s, vmulq_f32(y, am));
  }
  phase += (uint32_t)s * frequency;
  for (; s < size; ++s) {
    phase += frequency;
    uint32_t q = phase;
    if (mod) q += static_cast<uint32_t>((mod_a ? __builtin_fmaf(mod[s], mod_a[s], 32.0f) : mod[s] + 32.0f) * kScale) * 64;
    const float y = SinePM(q, -32.0f);
    if (out_a) out[s] = y, out_a[s] = amplitude;
    else if (additive) out[s] = __builtin_fmaf(y, amplitude, out[s]);
    else out[s] = y * amplitude;
    amplitude += increment;
  }
  op->phase = phase, op->amplitude = amplitude;
}

const size_t kChainMax = 64;   // the engine asks for 24 at most (kBlockSize x kNumSixOpVoices)

template<int n, bool external, bool additive>
void RenderChainNeon(Operator* ops, const float* f, const float* a, const float* modulation, float* out, size_t size) {
  float temp[2][2][kChainMax];   // a render core's stack: 1 KB
  const float scale = 1.0f / float(size);
  const float* mod = external ? modulation : nullptr;
  const float* mod_a = nullptr;
  for (int i = 0; i < n; ++i) {
    const uint32_t frequency = static_cast<uint32_t>(std::min(f[i], 0.5f) * 4294967296.0f);
    const float increment = (std::min(a[i], 4.0f) - ops[i].amplitude) * scale;
    const bool last = i == n - 1;
    float* o = last ? out : temp[i & 1][0];
    float* oa = last ? nullptr : temp[i & 1][1];
    RenderOneNeon(&ops[i], frequency, increment, mod, mod_a, o, oa, size, last && additive);
    mod = o, mod_a = oa;
  }
}
#endif

template<int n, int modulation_source, bool additive>
void RenderOperators(
    Operator* ops,
    const float* f,
    const float* a,
    float* fb_state,
    int fb_amount,
    const float* modulation,
    float* out,
    size_t size) {
#ifdef RYTM_FM_NEON
  if (modulation_source < Operator::MODULATION_SOURCE_FEEDBACK && size <= kChainMax) {
    RenderChainNeon<n, modulation_source == Operator::MODULATION_SOURCE_EXTERNAL, additive>(ops, f, a, modulation, out, size);
    return;
  }
#endif
  RenderOperatorsC<n, modulation_source, additive>(ops, f, a, fb_state, fb_amount, modulation, out, size);
}
