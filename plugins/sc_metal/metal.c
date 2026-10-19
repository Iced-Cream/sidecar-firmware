// plugins/sc_metal/metal.c - METAL: metallic FM percussion (bells, cowbells, clanks).
// SPDX-License-Identifier: MIT
// Copyright 2026 trigcondition and contributors
//
// Three sine operators: M2 (with self-feedback) modulates M1 and the carrier,
// M1 modulates the carrier. RATIO picks a set of inharmonic frequency ratios;
// the FM index has its own decay envelope (bright strike, mellow tail), a fast
// pitch drop at the strike, an amplitude decay, and a centred TONE (dark: a
// one-pole lowpass, bright: a one-pole highpass). One-shot: note_off is ignored.
//   P1 TUNE (64 = 0, +-24 st)  P2 RATIO (list)  P3 INDEX  P4 INDEX DECAY
//   P5 PITCH DROP  P6 DECAY  P7 TONE (64 = flat)  P8 LEVEL
//   P9 FEEDBACK  P10 INHARM (64 = as the ratio set; bends the modulator ratios)
// Per-voice state ~120 B: well under the 16384 B a voice has on the Teensy.
#include <math.h>
#include "rmp/rmp_api.h"
/* plugin.mk builds with -ffreestanding (no builtins): fabsf would be a libm call per use; this is one VABS */
#define fabsf(x) __builtin_fabsf(x)

typedef struct {
  float tune, idx, idx_k, drop_oct, dec_k, tone, level, fb, inharm;
  int32_t set;
  float note, vel, base;            /* base: carrier frequency / fs */
  float pc, p1, p2, o2a, o2b;       /* phases (cycles), M2's last two outputs (feedback) */
  float aenv, ienv, penv, lp;
  int32_t active;
} metal_state;

/* carrier ratio, M1 ratio, M2 ratio, M2 -> carrier share */
typedef struct { float c, m1, m2, m2c; } ratio_set;
#define N_SETS 8
static const ratio_set k_sets[N_SETS] = {
  {1.00f, 3.50f, 1.41f, 0.30f},   /* BELL: the classic 1 : 3.5 bell */
  {2.00f, 1.48f, 3.11f, 0.15f},   /* COWBELL: an octave up, two close inharmonic partials */
  {1.00f, 1.732f, 4.236f, 0.60f}, /* CLANK: sqrt 3, phi^3 */
  {0.50f, 1.414f, 0.707f, 0.50f}, /* GONG: an octave down, sqrt 2 */
  {2.00f, 7.13f, 2.00f, 0.20f},   /* CHIME */
  {1.00f, 2.31f, 3.73f, 0.45f},   /* PLATE */
  {3.00f, 4.77f, 9.10f, 0.10f},   /* GLASS */
  {1.00f, 14.0f, 1.00f, 0.25f},   /* TINE: a bright harmonic strike */
};

static const rmp_host *g_host;
static float g_fs = 48000.0f;

static inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   /* NaN -> lo */
static inline float n01(float p) { return clampf(p * (1.0f / 127.0f), 0.0f, 1.0f); }
static inline float fast_sin_cyc(float x) {   /* sin(2 pi x), parabolic + one correction, |err| ~ 1e-3 */
  int32_t i = (int32_t)x; if ((float)i > x) --i;
  x -= (float)i + 0.5f;
  float y = 8.0f * x - 16.0f * x * fabsf(x);
  y += 0.225f * y * (fabsf(y) - 1.0f);
  return -y;
}
static inline float wrap1(float x) { int32_t i = (int32_t)x; if ((float)i > x) --i; return x - (float)i; }
static inline float decay_k(float secs) { return expf(-6.9078f / (secs * g_fs)); }

/* list knob zones: index = (value x count) >> 7, the default in the middle of its zone */
static const float k_defaults[RMP_PARAMS] = {64, 8, 62, 42, 22, 58, 64, 100, 12, 64, 64, 64, 64, 64, 64, 64};

static int32_t metal_init(const rmp_host *h) {
  g_host = h;
  g_fs = h->sample_rate > 1000.0f ? h->sample_rate : 48000.0f;
  return 0;
}

static void retune(metal_state *s) {
  float f = 440.0f * expf((s->note + s->tune - 69.0f) * 0.05776227f) * k_sets[s->set].c;
  s->base = clampf(f / g_fs, 1e-5f, 0.2f);
}

static void metal_params(void *st, const float *p) {
  metal_state *s = st;
  s->tune = (n01(p[0]) - 0.5039f) * 48.0f;
  int32_t z = (int32_t)clampf(p[1], 0.0f, 127.0f);
  s->set = (z * N_SETS) >> 7;
  float ix = n01(p[2]);
  s->idx = 1.6f * ix * ix;                          /* peak index in cycles (x 2 pi = radians, up to ~10) */
  float id = n01(p[3]);
  s->idx_k = decay_k(0.01f + 1.6f * id * id);
  float dr = n01(p[4]);
  s->drop_oct = 2.0f * dr * dr;
  float d = n01(p[5]);
  s->dec_k = decay_k(0.04f + 6.0f * d * d);
  s->tone = (n01(p[6]) - 0.5039f) * 2.0f;
  float lv = n01(p[7]);
  s->level = lv * lv;
  s->fb = 0.25f * n01(p[8]);
  s->inharm = (n01(p[9]) - 0.5039f) * 2.0f;
  retune(s);
}

static void metal_voice_init(void *st) {
  metal_state *s = st;
  s->note = 60.0f;
  metal_params(s, k_defaults);
}

static void metal_note_on(void *st, float note, float vel) {
  metal_state *s = st;
  s->note = clampf(note, 0.0f, 127.0f);
  s->vel = clampf(vel, 0.0f, 1.0f);
  retune(s);
  s->pc = s->p1 = s->p2 = 0.0f;
  s->o2a = s->o2b = 0.0f;
  s->aenv = 1.0f; s->ienv = 1.0f; s->penv = 1.0f;
  s->active = 1;
}

static void metal_note_off(void *st) { (void)st; }   /* one-shot */

static void metal_render(void *st, float *out, int32_t n) {
  metal_state *s = st;
  if (!s->active) { for (int32_t i = 0; i < n; ++i) out[i] = 0.0f; return; }
  const ratio_set *rs = &k_sets[s->set];
  /* per block: pitch drop, ratios, tone filter */
  float pm = expf(s->drop_oct * s->penv * 0.6931472f);
  s->penv *= 0.80f;                                 /* ~22 ms at 12-sample blocks */
  float bend = expf(s->inharm * 0.25f * 0.6931472f); /* +-1/4 octave on M1, the inverse on M2 */
  float ic = s->base * pm;
  float i1 = ic * rs->m1 / rs->c * bend, i2 = ic * rs->m2 / rs->c / bend;
  i1 = clampf(i1, 0.0f, 0.45f); i2 = clampf(i2, 0.0f, 0.45f);
  float t = s->tone;
  int hp = t > 0.02f;                               /* a small dead zone around the centre: flat */
  float fc = hp ? 25.0f * expf((t - 0.02f) * 7.6f * 0.6931472f) : 18000.0f * expf((t < 0.0f ? t : 0.0f) * 5.0f * 0.6931472f);
  float a = 1.0f - expf(-6.2831853f * fc / g_fs);   /* one-pole */
  float vgain = 0.35f + 0.65f * s->vel;
  float vidx = s->idx * (0.5f + 0.5f * s->vel);
  float m2c = rs->m2c, fb = s->fb, dk = s->dec_k, ik = s->idx_k;
  float gain = s->level * vgain * 0.8f;
  float pc = s->pc, p1 = s->p1, p2 = s->p2, o2a = s->o2a, o2b = s->o2b;
  float aenv = s->aenv, ienv = s->ienv, lp = s->lp;
  for (int32_t i = 0; i < n; ++i) {
    float I = vidx * ienv;
    float o2 = fast_sin_cyc(p2 + fb * (o2a + o2b));  /* averaged feedback stays stable */
    o2b = o2a; o2a = o2;
    float o1 = fast_sin_cyc(p1 + 0.7f * I * o2);
    float oc = fast_sin_cyc(pc + I * o1 + m2c * I * o2);
    pc += ic; p1 += i1; p2 += i2;
    pc = pc >= 1.0f ? pc - 1.0f : pc; p1 = p1 >= 1.0f ? p1 - 1.0f : p1; p2 = p2 >= 1.0f ? p2 - 1.0f : p2;
    lp += a * (oc - lp);
    float y = (hp ? oc - lp : lp) * aenv * gain;
    aenv *= dk; ienv *= ik;
    if (!(y > -2.0f && y < 2.0f)) { y = 0.0f; lp = 0.0f; }
    out[i] = clampf(y, -1.0f, 1.0f);
  }
  if (aenv < 1e-5f) { aenv = 0.0f; s->active = 0; lp = 0.0f; }
  if (ienv < 1e-6f) ienv = 0.0f;
  if (s->penv < 1e-6f) s->penv = 0.0f;
  s->pc = wrap1(pc); s->p1 = wrap1(p1); s->p2 = wrap1(p2); s->o2a = o2a; s->o2b = o2b;
  s->aenv = aenv; s->ienv = ienv; s->lp = lp;
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 2u, sizeof(metal_state), 30,   /* estimated, not measured: ~140 cycles a sample (3 operators) on the M7;
     a Mac timing against sine_test (17) agrees within the guard's 2x. Check peak_permille in device-info */
  metal_init, metal_voice_init, metal_note_on, metal_note_off, metal_params, metal_render,
  {0, 0, 0, 0, 0, 0, 0, 0}};
