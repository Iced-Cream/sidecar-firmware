// plugins/sc_snare/snare.c - SNARE: a snare / noise drum designer.
// SPDX-License-Identifier: MIT
// Copyright 2026 trigcondition and contributors
//
// The body: two tuned sine oscillators (the second at BODY RATIO above the
// first, decaying a little faster) with a shared pitch envelope and a touch of
// saturation. The rattle: white noise through a TPT state-variable filter
// (bandpass at NOISE TONE plus some of its highpass) with its own decay. SNAP:
// a 2-3 ms transient (a falling high sine chirp plus a raw noise burst).
// BALANCE crossfades body and noise. One-shot: note_off is ignored; the note
// moves the body pitch (60 = 180 Hz).
//   P1 TUNE (64 = 0, +-24 st)  P2 BODY DECAY  P3 PITCH ENV  P4 NOISE TONE
//   P5 NOISE DECAY  P6 SNAP  P7 BALANCE (64 = even)  P8 LEVEL
//   P9 BODY RATIO  P10 NOISE RESO
// Per-voice state ~130 B: well under the 16384 B a voice has on the Teensy.
#include <math.h>
#include "rmp/rmp_api.h"
/* plugin.mk builds with -ffreestanding (no builtins): fabsf would be a libm call per use; this is one VABS */
#define fabsf(x) __builtin_fabsf(x)

typedef struct {
  float tune, bdec_k, bdec2_k, penv_oct, ntone, ndec_k, snap, bal_t, bal_n, level, ratio, nres;
  float note, vel, inc1;
  float ph1, ph2, ph3, benv, benv2, penv, nenv, senv, chirp;
  float ic1, ic2, hp1;
  uint32_t rng;
  int32_t active;
} snare_state;

static const rmp_host *g_host;
static float g_fs = 48000.0f;

static inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   /* NaN -> lo */
static inline float n01(float p) { return clampf(p * (1.0f / 127.0f), 0.0f, 1.0f); }
static inline float fast_tanh(float x) {
  x = clampf(x, -3.0f, 3.0f);
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
static inline float fast_sin_cyc(float x) {   /* sin(2 pi x), parabolic + one correction, |err| ~ 1e-3 */
  int32_t i = (int32_t)x; if ((float)i > x) --i;
  x -= (float)i + 0.5f;
  float y = 8.0f * x - 16.0f * x * fabsf(x);
  y += 0.225f * y * (fabsf(y) - 1.0f);
  return -y;
}
static inline float tan_pw(float x) {   /* tan(x) for 0..1.4 (filter prewarp), Pade */
  float x2 = x * x;
  return x * (945.0f - 105.0f * x2 + x2 * x2) / (945.0f - 420.0f * x2 + 15.0f * x2 * x2);
}
static inline float decay_k(float secs) { return expf(-6.9078f / (secs * g_fs)); }
/* white noise -1..1: an inline xorshift32 (seeded from host->random) */
static inline float noise(uint32_t *r) {
  uint32_t x = *r;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  *r = x;
  return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

static const float k_defaults[RMP_PARAMS] = {64, 40, 52, 74, 50, 64, 60, 100, 64, 30, 64, 64, 64, 64, 64, 64};

static int32_t snare_init(const rmp_host *h) {
  g_host = h;
  g_fs = h->sample_rate > 1000.0f ? h->sample_rate : 48000.0f;
  return 0;
}

static void retune(snare_state *s) {
  float f = 180.0f * expf((s->note - 60.0f + s->tune) * 0.05776227f);
  s->inc1 = clampf(f / g_fs, 1e-4f, 0.1f);
}

static void snare_params(void *st, const float *p) {
  snare_state *s = st;
  s->tune = (n01(p[0]) - 0.5039f) * 48.0f;
  float bd = n01(p[1]);
  float bsecs = 0.03f + 0.9f * bd * bd;
  s->bdec_k = decay_k(bsecs);
  s->bdec2_k = decay_k(bsecs * 0.65f);
  float pe = n01(p[2]);
  s->penv_oct = 2.0f * pe * pe;
  s->ntone = n01(p[3]);
  float nd = n01(p[4]);
  s->ndec_k = decay_k(0.04f + 1.0f * nd * nd);
  s->snap = n01(p[5]);
  float b = n01(p[6]);
  s->bal_t = clampf(2.0f * (1.0f - b), 0.0f, 1.0f);
  s->bal_n = clampf(2.0f * b, 0.0f, 1.0f);
  float lv = n01(p[7]);
  s->level = lv * lv;
  s->ratio = 1.25f + 1.0f * n01(p[8]);          /* 64: ~1.75 */
  s->nres = 1.8f - 1.5f * n01(p[9]);            /* SVF damping (1/Q) */
  retune(s);
}

static void snare_voice_init(void *st) {
  snare_state *s = st;
  s->note = 60.0f;
  s->rng = ((uint32_t)(uintptr_t)st * 2654435761u) | 1u;
  snare_params(s, k_defaults);
}

static void snare_note_on(void *st, float note, float vel) {
  snare_state *s = st;
  s->note = clampf(note, 0.0f, 127.0f);
  s->vel = clampf(vel, 0.0f, 1.0f);
  retune(s);
  uint32_t r = g_host->random(&s->rng);
  s->rng = r ? r : 0x9E3779B9u;
  s->ph1 = 0.0f; s->ph2 = 0.0f; s->ph3 = 0.0f;
  s->benv = s->benv2 = 1.0f;
  s->penv = 1.0f; s->nenv = 1.0f; s->senv = 1.0f;
  s->active = 1;
}

static void snare_note_off(void *st) { (void)st; }   /* one-shot */

static void snare_render(void *st, float *out, int32_t n) {
  snare_state *s = st;
  if (!s->active) { for (int32_t i = 0; i < n; ++i) out[i] = 0.0f; return; }
  /* per block: the pitch envelope, the noise filter */
  float pm = expf(s->penv_oct * s->penv * s->penv * 0.6931472f);
  s->penv *= 0.84f;                                  /* ~30 ms */
  float i1 = s->inc1 * pm, i2 = i1 * s->ratio;
  float fc = 900.0f * expf(s->ntone * 3.6f * 0.6931472f);   /* 0.9 .. 11 kHz */
  float g = tan_pw(3.14159265f * clampf(fc, 100.0f, 0.4f * g_fs) / g_fs), k = s->nres;
  float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
  float vg = 0.3f + 0.7f * s->vel;
  float gt = s->bal_t * vg * s->level, gn = s->bal_n * vg * s->level;
  float snap = s->snap * vg * s->level;
  float bk = s->bdec_k, bk2 = s->bdec2_k, nk = s->ndec_k;
  float ph1 = s->ph1, ph2 = s->ph2, ph3 = s->ph3, benv = s->benv, benv2 = s->benv2, nenv = s->nenv, senv = s->senv;
  float ic1 = s->ic1, ic2 = s->ic2, hp1 = s->hp1;
  uint32_t rng = s->rng;
  for (int32_t i = 0; i < n; ++i) {
    float body = fast_tanh(1.4f * (benv * fast_sin_cyc(ph1) + 0.55f * benv2 * fast_sin_cyc(ph2)));
    ph1 += i1; if (ph1 >= 1.0f) ph1 -= 1.0f;
    ph2 += i2; if (ph2 >= 1.0f) ph2 -= 1.0f;
    float w = noise(&rng);
    float v3 = w - ic2;
    float v1 = a1 * ic1 + a2 * v3;
    float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
    float nz = (1.6f * k * v1 + 0.6f * (w - k * v1 - v2)) * nenv;   /* bandpass (unity peak) + highpass */
    /* snap: a falling chirp from ~5 kHz plus the raw noise, highpassed */
    float sc = fast_sin_cyc(ph3) + 0.6f * w;
    ph3 += 0.02f + 0.09f * senv; if (ph3 >= 1.0f) ph3 -= 1.0f;
    hp1 += 0.25f * (sc - hp1);
    float sn = (sc - hp1) * senv;
    benv *= bk; benv2 *= bk2; nenv *= nk; senv *= 0.985f;   /* senv: ~1.4 ms time constant */
    float y = 0.7f * gt * body + 0.6f * gn * nz + 0.5f * snap * sn;
    if (!(y > -4.0f && y < 4.0f)) { y = 0.0f; ic1 = ic2 = hp1 = 0.0f; }
    out[i] = clampf(y, -1.0f, 1.0f);
  }
  if (senv < 1e-6f) senv = 0.0f;
  if (s->penv < 1e-6f) s->penv = 0.0f;
  if (benv < 1e-5f && nenv < 1e-5f) { benv = benv2 = nenv = 0.0f; s->active = 0; ic1 = ic2 = hp1 = 0.0f; }
  s->ph1 = ph1; s->ph2 = ph2; s->ph3 = ph3; s->benv = benv; s->benv2 = benv2; s->nenv = nenv; s->senv = senv;
  s->ic1 = ic1; s->ic2 = ic2; s->hp1 = hp1; s->rng = rng;
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 2u, sizeof(snare_state), 25,   /* estimated, not measured: ~200 cycles a sample on the M7;
     a Mac timing against sine_test (17) agrees within the guard's 2x. Check peak_permille in device-info */
  snare_init, snare_voice_init, snare_note_on, snare_note_off, snare_params, snare_render,
  {0, 0, 0, 0, 0, 0, 0, 0}};
