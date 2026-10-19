// plugins/sc_pluck/pluck.c - PLUCK: a Karplus-Strong plucked string.
// SPDX-License-Identifier: MIT
// Copyright 2026 trigcondition and contributors
//
// A delay-line loop (2048 samples: down to ~24 Hz at 48 kHz) with a one-pole
// damping lowpass, two first-order allpasses for STRETCH (a stiff string:
// partials spread sharp), a first-order allpass for the fractional delay, and
// a loop gain set from DECAY (the fundamental's T60). The loop is tuned from
// the phase delay of every filter at the note's frequency, so the pitch is the
// note at any damping or stretch. The excitation: a noise burst one period
// long, coloured by a one-pole lowpass (COLOUR, brighter with velocity). PICK
// POSITION is a comb (1 - z^-P), applied at the output tap (it commutes with
// the loop, so no second buffer). BODY adds two resonances of a small wooden
// box. A new note re-excites the ringing string; note_off is ignored.
//   P1 TUNE (64 = 0, +-12 st)  P2 DECAY  P3 DAMPING  P4 COLOUR  P5 PICK POS
//   P6 BODY  P7 STRETCH  P8 LEVEL
// Per-voice state: 2048 floats + ~100 B = ~8.3 KB, under the 16384 B a voice
// has on the Teensy (its own 16 KB engine scratch).
#include <math.h>
#include "rmp/rmp_api.h"
/* plugin.mk builds with -ffreestanding (no builtins): fabsf would be a libm call per use; this is one VABS */
#define fabsf(x) __builtin_fabsf(x)

#define LINE_N 2048
#define LINE_MASK (LINE_N - 1)

typedef struct {
  float line[LINE_N];
  float tune, t60, damp, damp_c, colour, pos, body, stretch, level;
  float note, vel;
  /* derived: loop */
  int32_t N, P, w, burst;
  float fa, sa, g, comb_gain, exc_c, exc_gain;
  /* filter states */
  float ap_x1, ap_y1, lp, s1x, s1y, s2x, s2y, exc_lp;
  float b1a, b1b, b2a, b2b;   /* body SVF states */
  uint32_t rng;
  int32_t active, quiet;
} pluck_state;

static const rmp_host *g_host;
static float g_fs = 48000.0f;
static float g_body[2][3];   /* body resonators' SVF a1, a2, a3 (written once in init) */
static const float k_body_k[2] = {0.45f, 0.6f};

static inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   /* NaN -> lo */
static inline float n01(float p) { return clampf(p * (1.0f / 127.0f), 0.0f, 1.0f); }
static inline float fast_tanh(float x) {
  x = clampf(x, -3.0f, 3.0f);
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
static inline float tan_pw(float x) {
  float x2 = x * x;
  return x * (945.0f - 105.0f * x2 + x2 * x2) / (945.0f - 420.0f * x2 + 15.0f * x2 * x2);
}
static inline float noise(uint32_t *r) {
  uint32_t x = *r;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  *r = x;
  return (float)(int32_t)x * (1.0f / 2147483648.0f);
}
/* sin(x), reduced to -pi/2..pi/2, Taylor to x^11 (|err| < 1e-7): for tuning, outside render (no libm trig tables) */
static float sin_acc(float x) {
  const float pi = 3.14159265f, tau = 6.2831853f;
  x -= tau * (float)(int32_t)(x / tau);
  if (x > pi) x -= tau; else if (x < -pi) x += tau;
  if (x > 0.5f * pi) x = pi - x; else if (x < -0.5f * pi) x = -pi - x;
  float x2 = x * x;
  return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f)))));
}
static inline float cos_acc(float x) { return sin_acc(x + 1.5707963f); }
/* phase delay (samples) at w of a first-order allpass (a + z^-1) / (1 + a z^-1) */
static float ap_delay(float a, float w) {
  float sw = sin_acc(w), cw = cos_acc(w);
  float ph = atan2f(-sw, a + cw) - atan2f(-a * sw, 1.0f + a * cw);
  return -ph / w;
}

static const float k_defaults[RMP_PARAMS] = {64, 64, 40, 80, 30, 40, 0, 100, 64, 64, 64, 64, 64, 64, 64, 64};

static int32_t pluck_init(const rmp_host *h) {
  g_host = h;
  g_fs = h->sample_rate > 1000.0f ? h->sample_rate : 48000.0f;
  static const float fr[2] = {118.0f, 262.0f};
  for (int b = 0; b < 2; ++b) {
    float g = tan_pw(3.14159265f * fr[b] / g_fs), k = k_body_k[b];
    float a1 = 1.0f / (1.0f + g * (g + k));
    g_body[b][0] = a1; g_body[b][1] = g * a1; g_body[b][2] = g * g * a1;
  }
  return 0;
}

/* the loop's integer delay, fractional allpass, gain and comb from note + params */
static void retune(pluck_state *s) {
  float f = 440.0f * expf((s->note + s->tune - 69.0f) * 0.05776227f);
  f = clampf(f, g_fs / (float)(LINE_N - 8), g_fs / 8.0f);
  float D = g_fs / f, w = 6.2831853f * f / g_fs;
  /* damping one-pole c / (1 - (1-c) z^-1), its cutoff in harmonics of the note (2.5 .. ~60), so a
     damping setting sounds alike on every note; its phase delay and gain at w */
  float dm = 1.0f - s->damp;
  float fc = f * (2.5f + 60.0f * dm * dm);
  float c = fc >= 0.45f * g_fs ? 1.0f : 1.0f - expf(-6.2831853f * fc / g_fs), q = 1.0f - c;
  s->damp_c = c;
  float re = 1.0f - q * cos_acc(w), im = q * sin_acc(w);
  float d_lp = atan2f(im, re) / w;
  float mag_lp = c / sqrtf(re * re + im * im);
  float d_st = 2.0f * ap_delay(s->sa, w);
  float r = D - d_lp - d_st;
  int32_t N = (int32_t)(r - 0.1f);
  if (N < 2) N = 2;
  if (N > LINE_N - 2) N = LINE_N - 2;
  float frac = r - (float)N;
  float a = (1.0f - frac) / (1.0f + frac);
  frac += frac - ap_delay(a, w);                       /* one correction step at the note's frequency */
  a = clampf((1.0f - frac) / (1.0f + frac), -0.95f, 0.95f);
  s->N = N; s->fa = a;
  float per = expf(-6.9078f * D / (s->t60 * g_fs));    /* the fundamental's T60 */
  s->g = clampf(per / mag_lp, 0.0f, 0.99995f);
  int32_t P = (int32_t)(s->pos * D + 0.5f);
  s->P = P < 1 ? 1 : (P > LINE_N - 1 ? LINE_N - 1 : P);
  float sp = sin_acc(3.14159265f * (float)s->P / D);
  s->comb_gain = 0.5f / clampf(fabsf(sp), 0.2f, 1.0f);
}

static void pluck_params(void *st, const float *p) {
  pluck_state *s = st;
  s->tune = (n01(p[0]) - 0.5039f) * 24.0f;
  float d = n01(p[1]);
  s->t60 = 0.12f + 10.0f * d * d;
  s->damp = n01(p[2]);
  float co = n01(p[3]);
  s->colour = co;
  s->pos = 0.03f + 0.47f * n01(p[4]);
  s->body = n01(p[5]);
  float stv = n01(p[6]);
  s->sa = -0.75f * stv;                                /* negative allpass coefficient: dispersion */
  float lv = n01(p[7]);
  s->level = lv * lv;
  retune(s);
}

static void pluck_voice_init(void *st) {
  pluck_state *s = st;
  s->note = 60.0f;
  s->rng = ((uint32_t)(uintptr_t)st * 2654435761u) | 1u;
  pluck_params(s, k_defaults);
}

static void pluck_note_on(void *st, float note, float vel) {
  pluck_state *s = st;
  s->note = clampf(note, 0.0f, 127.0f);
  s->vel = clampf(vel, 0.0f, 1.0f);
  retune(s);
  uint32_t r = g_host->random(&s->rng);
  s->rng = r ? r : 0x9E3779B9u;
  /* excitation: one period of coloured noise; velocity brightens it */
  float cv = clampf(s->colour * (0.6f + 0.5f * s->vel), 0.0f, 1.0f);
  float c = 0.03f + 0.97f * cv * cv;
  s->exc_c = c;
  s->exc_gain = (0.25f + 0.75f * s->vel) * sqrtf((2.0f - c) / c) * 0.6f;
  s->exc_gain = clampf(s->exc_gain, 0.0f, 3.0f);
  s->burst = s->N + 1;
  s->active = 1; s->quiet = 0;
}

static void pluck_note_off(void *st) { (void)st; }   /* the string rings out */

static void pluck_render(void *st, float *out, int32_t n) {
  pluck_state *s = st;
  if (!s->active) { for (int32_t i = 0; i < n; ++i) out[i] = 0.0f; return; }
  float *L = s->line;
  int32_t w = s->w, N = s->N, P = s->P, burst = s->burst;
  float fa = s->fa, sa = s->sa, g = s->g, c = s->damp_c, cg = s->comb_gain * s->level;
  float ec = s->exc_c, eg = s->exc_gain, elp = s->exc_lp;
  float ax = s->ap_x1, ay = s->ap_y1, lp = s->lp, s1x = s->s1x, s1y = s->s1y, s2x = s->s2x, s2y = s->s2y;
  float b1a = s->b1a, b1b = s->b1b, b2a = s->b2a, b2b = s->b2b;
  float body = s->body * 1.2f, dry = 1.0f - 0.4f * s->body;
  const float *B1 = g_body[0], *B2 = g_body[1];
  uint32_t rng = s->rng;
  float peak = 0.0f;
  for (int32_t i = 0; i < n; ++i) {
    float x = L[(w - N) & LINE_MASK];
    float y = fa * x + ax - fa * ay; ax = x; ay = y;          /* fractional allpass */
    lp += c * (y - lp);                                       /* damping */
    float t = sa * lp + s1x - sa * s1y; s1x = lp; s1y = t;    /* stretch allpass 1 */
    float u = sa * t + s2x - sa * s2y; s2x = t; s2y = u;      /* stretch allpass 2 */
    float v = g * u;
    if (burst > 0) { elp += ec * (noise(&rng) - elp); v += eg * elp; --burst; }
    L[w & LINE_MASK] = v;
    float o = (v - L[(w - P) & LINE_MASK]) * cg;              /* pick position comb */
    w = (w + 1) & LINE_MASK;
    /* body: two SVF bandpasses */
    float v3 = o - b1b, v1 = B1[0] * b1a + B1[1] * v3, v2 = b1b + B1[1] * b1a + B1[2] * v3;
    b1a = 2.0f * v1 - b1a; b1b = 2.0f * v2 - b1b;
    float bp = k_body_k[0] * v1;
    v3 = o - b2b; v1 = B2[0] * b2a + B2[1] * v3; v2 = b2b + B2[1] * b2a + B2[2] * v3;
    b2a = 2.0f * v1 - b2a; b2b = 2.0f * v2 - b2b;
    bp += 0.7f * k_body_k[1] * v1;
    float r = fast_tanh(dry * o + body * 2.5f * bp);
    if (!(r > -2.0f && r < 2.0f)) { r = 0.0f; lp = ax = ay = s1x = s1y = s2x = s2y = b1a = b1b = b2a = b2b = 0.0f; }
    out[i] = r;
    float m = fabsf(v); if (m > peak) peak = m;
  }
  /* idle after ~0.25 s below -100 dB in the loop */
  if (burst == 0 && peak < 1e-5f) {
    s->quiet += n;
    if (s->quiet > (int32_t)(0.25f * g_fs)) {   /* what is left in the line is inaudible; a new note adds to it */
      s->active = 0;
      lp = ax = ay = s1x = s1y = s2x = s2y = b1a = b1b = b2a = b2b = elp = 0.0f;
    }
  } else {
    s->quiet = 0;
  }
  s->w = w; s->burst = burst; s->exc_lp = elp; s->rng = rng;
  s->ap_x1 = ax; s->ap_y1 = ay; s->lp = lp; s->s1x = s1x; s->s1y = s1y; s->s2x = s2x; s->s2y = s2y;
  s->b1a = b1a; s->b1b = b1b; s->b2a = b2a; s->b2b = b2b;
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 2u, sizeof(pluck_state), 25,   /* estimated, not measured: ~130-200 cycles a sample on the M7;
     a Mac timing against sine_test (17) agrees within the guard's 2x. Check peak_permille in device-info */
  pluck_init, pluck_voice_init, pluck_note_on, pluck_note_off, pluck_params, pluck_render,
  {0, 0, 0, 0, 0, 0, 0, 0}};
