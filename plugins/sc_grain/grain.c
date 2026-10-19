// plugins/sc_grain/grain.c - GRAIN: a granular texture over a built-in sample.
// SPDX-License-Identifier: MIT
// Copyright 2026 trigcondition and contributors
// (The built-in sample, texture_data.h, is CC0 1.0: see LICENSE-texture.txt and make_texture.py.)
//
// Up to 8 Hann-windowed grains per voice read the texture (1.0 s, 10 kHz,
// mu-law, loopable) with linear interpolation. A grain starts every
// 1/DENSITY s (+-30% jitter) at POSITION, scattered by SPRAY, lasts SIZE,
// plays at the note (60 = the texture's own pitch, C4) + PITCH, detuned at
// random by up to SPREAD, and backwards with REVERSE probability. Random
// numbers come from host->random on the voice's own state. The sum goes
// through a one-pole TONE lowpass and a gate-held attack/release envelope.
//   P1 POSITION  P2 SPRAY  P3 SIZE  P4 DENSITY  P5 PITCH (64 = 0, +-24 st)
//   P6 SPREAD  P7 REVERSE  P8 LEVEL
//   P9 ATTACK  P10 RELEASE  P11 TONE
// Per-voice state ~240 B, far under the 16384 B a voice has on the Teensy;
// the texture is 10000 B of read-only data in the image, the mu-law decode
// table 1 KB written once in init().
#include <math.h>
#include "rmp/rmp_api.h"
/* plugin.mk builds with -ffreestanding (no builtins): fabsf would be a libm call per use; this is one VABS */
#define fabsf(x) __builtin_fabsf(x)
#include "texture_data.h"

#define MAX_GRAINS 8

typedef struct { float pos, inc, t, dt, amp; } grain_t;   /* dt == 0: free */

typedef struct {
  grain_t g[MAX_GRAINS];
  float position, spray, size_s, density, transpose, spread, revp, level, att_k, rel_k, tone_c, norm;
  float note, vel, env, next, lp;
  uint32_t rng;
  int32_t gate, active;
} grain_state;

static const rmp_host *g_host;
static float g_fs = 48000.0f;
static float g_dec[256];   /* mu-law decode, written once in init() */

static inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   /* NaN -> lo */
static inline float n01(float p) { return clampf(p * (1.0f / 127.0f), 0.0f, 1.0f); }
static inline float fast_sin_cyc(float x) {   /* sin(2 pi x), parabolic + one correction, |err| ~ 1e-3 */
  int32_t i = (int32_t)x; if ((float)i > x) --i;
  x -= (float)i + 0.5f;
  float y = 8.0f * x - 16.0f * x * fabsf(x);
  y += 0.225f * y * (fabsf(y) - 1.0f);
  return -y;
}
static inline float fast_tanh(float x) {
  x = clampf(x, -3.0f, 3.0f);
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
static inline float rnd01(grain_state *s) { return (float)(g_host->random(&s->rng) >> 8) * (1.0f / 16777216.0f); }

static const float k_defaults[RMP_PARAMS] = {40, 24, 64, 64, 64, 20, 0, 100, 20, 60, 110, 64, 64, 64, 64, 64};

static int32_t grain_init(const rmp_host *h) {
  g_host = h;
  g_fs = h->sample_rate > 1000.0f ? h->sample_rate : 48000.0f;
  for (int q = 0; q < 256; ++q) {
    float y = (float)q / 127.5f - 1.0f, a = fabsf(y);
    float x = (expf(a * 5.5451774f) - 1.0f) / 255.0f;    /* (1 + 255)^|y| = e^(|y| ln 256) */
    g_dec[q] = y < 0.0f ? -x : x;
  }
  return 0;
}

static void grain_params(void *st, const float *p) {
  grain_state *s = st;
  s->position = n01(p[0]);
  s->spray = n01(p[1]);
  float sz = n01(p[2]);
  s->size_s = 0.012f + 0.5f * sz * sz;
  float de = n01(p[3]);
  s->density = 1.5f + 100.0f * de * de;
  s->transpose = (n01(p[4]) - 0.5039f) * 48.0f;
  float sp = n01(p[5]);
  s->spread = 7.0f * sp * sp;                           /* semitones, either way */
  s->revp = n01(p[6]);
  float lv = n01(p[7]);
  s->level = lv * lv;
  float a = n01(p[8]), r = n01(p[9]);
  float att = 0.003f + 2.0f * a * a, rel = 0.02f + 4.0f * r * r;
  s->att_k = 1.0f - expf(-3.0f / (att * g_fs));
  s->rel_k = expf(-6.9078f / (rel * g_fs));
  float tn = n01(p[10]);
  s->tone_c = 1.0f - expf(-6.2831853f * (300.0f * expf(tn * 4.2f * 0.6931472f)) / g_fs);   /* 300 Hz .. ~5.5 kHz */
  if (tn > 0.97f) s->tone_c = 1.0f;
  /* overlapping grains add as noise: scale by 1/sqrt(overlap) */
  float ov = s->density * s->size_s;
  if (ov > (float)MAX_GRAINS) ov = (float)MAX_GRAINS;
  s->norm = 2.4f / sqrtf(ov < 1.0f ? 1.0f : ov);
}

static void grain_voice_init(void *st) {
  grain_state *s = st;
  s->note = 60.0f;
  s->rng = ((uint32_t)(uintptr_t)st * 2654435761u) | 1u;
  grain_params(s, k_defaults);
}

static void spawn(grain_state *s) {
  grain_t *g = 0;
  for (int k = 0; k < MAX_GRAINS; ++k) if (s->g[k].dt == 0.0f) { g = &s->g[k]; break; }
  float r = rnd01(s);
  s->next += g_fs / s->density * (0.7f + 0.6f * r);
  if (!g) return;                                       /* all 8 busy: skip this one */
  float pos = (s->position + s->spray * (rnd01(s) - 0.5f)) * (float)TEX_LEN;
  while (pos < 0.0f) pos += (float)TEX_LEN;
  while (pos >= (float)TEX_LEN) pos -= (float)TEX_LEN;
  float semis = s->note - 60.0f + s->transpose + s->spread * (2.0f * rnd01(s) - 1.0f);
  float inc = expf(semis * 0.05776227f) * ((float)TEX_RATE / g_fs);
  inc = clampf(inc, 0.0f, 8.0f);
  if (rnd01(s) < s->revp) inc = -inc;
  g->pos = pos; g->inc = inc; g->t = 0.0f;
  g->dt = 1.0f / (s->size_s * g_fs);
  g->amp = 0.75f + 0.25f * rnd01(s);
}

static void grain_note_on(void *st, float note, float vel) {
  grain_state *s = st;
  s->note = clampf(note, 0.0f, 127.0f);
  s->vel = clampf(vel, 0.0f, 1.0f);
  s->gate = 1;
  if (!s->active) { s->env = 0.0f; s->next = 0.0f; }
  s->active = 1;
}

static void grain_note_off(void *st) { ((grain_state *)st)->gate = 0; }

static void grain_render(void *st, float *out, int32_t n) {
  grain_state *s = st;
  if (!s->active) { for (int32_t i = 0; i < n; ++i) out[i] = 0.0f; return; }
  float ek = s->gate ? s->att_k : 0.0f, rk = s->gate ? 1.0f : s->rel_k;
  float gain = s->level * s->norm * (0.4f + 0.6f * s->vel);
  float env = s->env, lp = s->lp, tc = s->tone_c;
  const float L = (float)TEX_LEN;
  for (int32_t i = 0; i < n; ++i) {
    if (s->gate) { s->next -= 1.0f; if (s->next <= 0.0f) spawn(s); }
    float sum = 0.0f;
    for (int k = 0; k < MAX_GRAINS; ++k) {
      grain_t *g = &s->g[k];
      if (g->dt == 0.0f) continue;
      int32_t j = (int32_t)g->pos;
      float f = g->pos - (float)j;
      int32_t j1 = j + 1 < TEX_LEN ? j + 1 : 0;
      float a = g_dec[tex_mulaw[j]], b = g_dec[tex_mulaw[j1]];
      float w = fast_sin_cyc(0.5f * g->t);              /* Hann: sin^2(pi t) */
      sum += (a + f * (b - a)) * w * w * g->amp;
      g->pos += g->inc;
      if (g->pos >= L) g->pos -= L; else if (g->pos < 0.0f) g->pos += L;
      g->t += g->dt;
      if (g->t >= 1.0f) g->dt = 0.0f;
    }
    lp += tc * (sum - lp);
    env += (1.0f - env) * ek; env *= rk;
    float y = lp * gain * env;
    if (!(y > -8.0f && y < 8.0f)) { y = 0.0f; lp = 0.0f; }
    out[i] = fast_tanh(y);
  }
  if (!s->gate && env < 1e-5f) {
    env = 0.0f; s->active = 0; lp = 0.0f;
    for (int k = 0; k < MAX_GRAINS; ++k) s->g[k].dt = 0.0f;
  }
  s->env = env; s->lp = lp;
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 2u, sizeof(grain_state), 50,   /* estimated, not measured: ~350 cycles a sample with all 8 grains, + a grain start on the M7;
     a Mac timing against sine_test (17) agrees within the guard's 2x. Check peak_permille in device-info */
  grain_init, grain_voice_init, grain_note_on, grain_note_off, grain_params, grain_render,
  {0, 0, 0, 0, 0, 0, 0, 0}};
