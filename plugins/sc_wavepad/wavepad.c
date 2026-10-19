// plugins/sc_wavepad/wavepad.c - WAVEPAD: a morphing wavetable pad.
// SPDX-License-Identifier: MIT
// Copyright 2026 trigcondition and contributors
//
// Five single-cycle shapes, built band-limited (additive, sigma-smoothed) in
// init() into one static table: SINE, TRIANGLE, SQUARE, SAW, FORMANT ("ah").
// MORPH sweeps across them (each read crossfades the two neighbours), an LFO
// moves the morph, three voices are detuned around the note plus a sine sub an
// octave down, a gentle 2-pole TPT state-variable lowpass that opens with the
// envelope, and a slow attack/release envelope held by the gate.
//   P1 TUNE (64 = 0, +-12 st)  P2 MORPH  P3 DETUNE  P4 CUTOFF  P5 ATTACK
//   P6 RELEASE  P7 LFO RATE  P8 LEVEL
//   P9 LFO DEPTH  P10 RESO  P11 SUB  P12 VELOCITY (amount)
// Per-voice state ~100 B, far under the 16384 B a voice has on the Teensy; the
// shared table (5 x 257 floats, 5 KB bss) is written once in init().
#include <math.h>
#include "rmp/rmp_api.h"
/* plugin.mk builds with -ffreestanding (no builtins): fabsf would be a libm call per use; this is one VABS */
#define fabsf(x) __builtin_fabsf(x)

#define WT_N 256
#define WT_SHAPES 5

typedef struct {
  float tune, morph, detune, cut, att_k, rel_k, lfo_inc, level, lfo_depth, res_k, sub, velamt;
  float note, inc[3], ph[3], sub_ph, lfo_ph;
  float env, vel_gain, ic1, ic2;
  int32_t gate, active;
} pad_state;

static const rmp_host *g_host;
static float g_fs = 48000.0f;
static int16_t g_wt[WT_SHAPES][WT_N + 1];   /* Q15 (x 1/16384: the peak may reach 1.6), written once in init() */

static inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   /* NaN -> lo */
static inline float n01(float p) { return clampf(p * (1.0f / 127.0f), 0.0f, 1.0f); }
static inline float tan_pw(float x) {   /* tan(x) for 0..1.4 (filter prewarp), Pade, rel err < 3e-5 */
  float x2 = x * x;
  return x * (945.0f - 105.0f * x2 + x2 * x2) / (945.0f - 420.0f * x2 + 15.0f * x2 * x2);
}
static inline float fast_tanh(float x) {   /* Pade-style, exact +-1 at |x| = 3 */
  x = clampf(x, -3.0f, 3.0f);
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
static inline float fast_sin_cyc(float x) {   /* sin(2 pi x), parabolic + one correction, |err| ~ 1e-3 */
  int32_t i = (int32_t)x; if ((float)i > x) --i;
  x -= (float)i + 0.5f;                          /* -0.5..0.5, shifted by half a cycle */
  float y = 8.0f * x - 16.0f * x * fabsf(x);
  y += 0.225f * y * (fabsf(y) - 1.0f);
  return -y;                                     /* the half-cycle shift flips the sign */
}

/* sin(x) for init(): reduced to -pi/2..pi/2, Taylor to x^11 (|err| < 1e-7); no libm trig tables */
static float sin_acc(float x) {
  const float pi = 3.14159265f, tau = 6.2831853f;
  x -= tau * (float)(int32_t)(x / tau);
  if (x > pi) x -= tau; else if (x < -pi) x += tau;
  if (x > 0.5f * pi) x = pi - x; else if (x < -0.5f * pi) x = -pi - x;
  float x2 = x * x;
  return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f * (1.0f - x2 / 110.0f)))));
}

/* one shape: additive up to harmonic `top`, amplitude amp(k), Lanczos sigma against ringing */
static void build(int16_t *out, int top, int shape) {
  float t[WT_N + 1];
  float peak = 1e-9f;
  for (int i = 0; i < WT_N; ++i) t[i] = 0.0f;
  for (int k = 1; k <= top; ++k) {
    float a;
    switch (shape) {
      case 0: a = k == 1 ? 1.0f : 0.0f; break;                                         /* sine */
      case 1: a = (k & 1) ? ((((k - 1) >> 1) & 1) ? -1.0f : 1.0f) / (float)(k * k) : 0.0f; break;   /* triangle */
      case 2: a = (k & 1) ? 1.0f / (float)k : 0.0f; break;                             /* square */
      case 3: a = 1.0f / (float)k; break;                                               /* saw */
      default: {                                                                        /* formant: two vowel peaks */
        float d1 = ((float)k - 4.5f) / 1.6f, d2 = ((float)k - 9.0f) / 2.2f;
        a = expf(-d1 * d1) + 0.6f * expf(-d2 * d2) + 0.25f / (float)k;
      } break;
    }
    if (a == 0.0f) continue;
    float x = 3.14159265f * (float)k / (float)(top + 1);
    float sigma = sin_acc(x) / x;
    a *= sigma;
    /* sin(k w i) by the rotation recurrence */
    float w = 6.2831853f * (float)k / (float)WT_N;
    float c2 = 2.0f * sin_acc(w + 1.5707963f), s0 = 0.0f, s1 = sin_acc(-w);
    for (int i = 0; i < WT_N; ++i) {
      float sn = c2 * s0 - s1;     /* s0 = sin(k w i), s1 = sin(k w (i-1)) */
      t[i] += a * s0;
      s1 = s0; s0 = sn;
    }
  }
  /* equal loudness across the morph: RMS 0.5, the peak at most 1.6 */
  float sum = 0.0f;
  for (int i = 0; i < WT_N; ++i) { if (fabsf(t[i]) > peak) peak = fabsf(t[i]); sum += t[i] * t[i]; }
  float rms = sqrtf(sum / (float)WT_N), gain = 0.5f / (rms + 1e-9f);
  if (gain * peak > 1.6f) gain = 1.6f / peak;
  for (int i = 0; i < WT_N; ++i) {
    float v = t[i] * gain * 16384.0f;
    out[i] = (int16_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
  }
  out[WT_N] = out[0];
}

static const float k_defaults[RMP_PARAMS] = {64, 44, 50, 78, 28, 72, 36, 100, 44, 24, 30, 64, 64, 64, 64, 64};

static int32_t pad_init(const rmp_host *h) {
  g_host = h;
  g_fs = h->sample_rate > 1000.0f ? h->sample_rate : 48000.0f;
  for (int s = 0; s < WT_SHAPES; ++s) build(g_wt[s], 48, s);
  return 0;
}

static void retune(pad_state *s) {
  float f = 440.0f * expf((s->note + s->tune - 69.0f) * 0.05776227f);
  float c = f / g_fs;
  float d = expf(s->detune * 0.05776227f);       /* detune in semitones */
  s->inc[0] = c; s->inc[1] = c * d; s->inc[2] = c / d;
}

static void pad_params(void *st, const float *p) {
  pad_state *s = st;
  s->tune = (n01(p[0]) - 0.5039f) * 24.0f;
  s->morph = n01(p[1]) * (float)(WT_SHAPES - 1);
  float dt = n01(p[2]);
  s->detune = 0.6f * dt * dt;                    /* up to 60 cents */
  s->cut = n01(p[3]);
  float a = n01(p[4]), r = n01(p[5]);
  float att = 0.004f + 5.0f * a * a, rel = 0.03f + 8.0f * r * r;
  s->att_k = 1.0f - expf(-3.0f / (att * g_fs));  /* ~95% of the way in `att` */
  s->rel_k = expf(-6.9078f / (rel * g_fs));
  float lr = n01(p[6]);
  s->lfo_inc = (0.03f + 7.0f * lr * lr) / g_fs;
  float lv = n01(p[7]);
  s->level = lv * lv;
  float ld = n01(p[8]);
  s->lfo_depth = 1.8f * ld;
  s->res_k = 1.6f - 1.3f * n01(p[9]);           /* SVF damping k = 1/Q: Q 0.6 .. 3.3 */
  s->sub = 0.8f * n01(p[10]);
  s->velamt = n01(p[11]);
  retune(s);
}

static void pad_voice_init(void *st) {
  pad_state *s = st;
  s->note = 60.0f;
  pad_params(s, k_defaults);
  /* each voice's own LFO and oscillator phases, from its address (no global written) */
  uint32_t seed = (uint32_t)(uintptr_t)st * 2654435761u;
  s->lfo_ph = (float)(seed >> 8) * (1.0f / 16777216.0f);
  s->ph[1] = (float)((seed * 3u) >> 8) * (1.0f / 16777216.0f);
  s->ph[2] = (float)((seed * 7u) >> 8) * (1.0f / 16777216.0f);
}

static void pad_note_on(void *st, float note, float vel) {
  pad_state *s = st;
  s->note = clampf(note, 0.0f, 127.0f);
  vel = clampf(vel, 0.0f, 1.0f);
  s->vel_gain = 1.0f - s->velamt * (1.0f - vel);
  s->gate = 1; s->active = 1;
  retune(s);
}

static void pad_note_off(void *st) { ((pad_state *)st)->gate = 0; }

static inline float wt_read(const int16_t *t, float ph) {   /* in units of 1/16384 */
  float x = ph * (float)WT_N;
  int32_t i = (int32_t)x;
  float f = x - (float)i, a = (float)t[i];
  return a + f * ((float)t[i + 1] - a);
}

static void pad_render(void *st, float *out, int32_t n) {
  pad_state *s = st;
  if (!s->active) { for (int32_t i = 0; i < n; ++i) out[i] = 0.0f; return; }
  /* per block: LFO -> morph, envelope -> cutoff, SVF coefficients */
  float lfo = fast_sin_cyc(s->lfo_ph);
  s->lfo_ph += s->lfo_inc * (float)n; if (s->lfo_ph >= 1.0f) s->lfo_ph -= 1.0f;
  float m = clampf(s->morph + s->lfo_depth * lfo, 0.0f, (float)(WT_SHAPES - 1) - 1e-4f);
  int32_t mi = (int32_t)m;
  float mf = m - (float)mi;
  const int16_t *ta = g_wt[mi], *tb = g_wt[mi + 1 < WT_SHAPES ? mi + 1 : mi];
  float fc = 60.0f * expf((8.1f * s->cut + 1.2f * (s->env - 1.0f)) * 0.6931472f);
  fc = clampf(fc, 30.0f, 0.4f * g_fs);
  float g = tan_pw(3.14159265f * fc / g_fs), k = s->res_k;
  float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
  float ek = s->gate ? s->att_k : 0.0f, rk = s->gate ? 1.0f : s->rel_k;
  float gain = s->level * s->vel_gain * (0.7f / 16384.0f);
  float env = s->env, ic1 = s->ic1, ic2 = s->ic2;
  float p0 = s->ph[0], p1 = s->ph[1], p2 = s->ph[2], ps = s->sub_ph;
  float i0 = s->inc[0], i1 = s->inc[1], i2 = s->inc[2], is = 0.5f * i0, sub = s->sub;
  for (int32_t i = 0; i < n; ++i) {
    float a = wt_read(ta, p0) + wt_read(ta, p1) + wt_read(ta, p2);
    float b = wt_read(tb, p0) + wt_read(tb, p1) + wt_read(tb, p2);
    float x = a + mf * (b - a) + sub * (1.5f * 16384.0f) * fast_sin_cyc(ps);
    p0 += i0; if (p0 >= 1.0f) p0 -= 1.0f;
    p1 += i1; if (p1 >= 1.0f) p1 -= 1.0f;
    p2 += i2; if (p2 >= 1.0f) p2 -= 1.0f;
    ps += is; if (ps >= 1.0f) ps -= 1.0f;
    /* SVF lowpass */
    float v3 = x - ic2;
    float v1 = a1 * ic1 + a2 * v3;
    float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
    env += (1.0f - env) * ek; env *= rk;
    float y = v2 * gain * env;
    if (!(y > -8.0f && y < 8.0f)) { y = 0.0f; ic1 = ic2 = 0.0f; }
    out[i] = fast_tanh(y);                       /* a soft ceiling for big chords of detune and resonance */
  }
  if (!s->gate && env < 1e-5f) { env = 0.0f; s->active = 0; ic1 = ic2 = 0.0f; }
  s->env = env; s->ic1 = ic1; s->ic2 = ic2;
  s->ph[0] = p0; s->ph[1] = p1; s->ph[2] = p2; s->sub_ph = ps;
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 2u, sizeof(pad_state), 30,   /* estimated, not measured: ~170-200 cycles a sample (6 table reads, SVF) on the M7;
     a Mac timing against sine_test (17) agrees within the guard's 2x. Check peak_permille in device-info */
  pad_init, pad_voice_init, pad_note_on, pad_note_off, pad_params, pad_render,
  {0, 0, 0, 0, 0, 0, 0, 0}};
