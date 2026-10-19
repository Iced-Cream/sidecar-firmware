// plugins/sc_acid/acid.c - ACID: a 303-style bass line voice.
// SPDX-License-Identifier: MIT
// Copyright 2026 trigcondition and contributors
//
// A polyBLEP saw/square blend into a 4-pole ladder (four TPT one-poles, the
// feedback solved zero-delay, the ladder input tanh-saturated), a filter
// envelope with env mod and decay, accent (velocity > 0.8: more cutoff through
// a slow "accent sweep" lag, more level, a shorter filter decay), slide (a
// note while the gate is still on glides there without retriggering the
// envelopes) and a drive stage.
//   P1 TUNE (64 = 0, +-12 st)  P2 WAVE (saw..square)  P3 CUTOFF  P4 RESO
//   P5 ENV MOD  P6 DECAY  P7 ACCENT  P8 LEVEL
//   P9 SLIDE (time)  P10 DRIVE  P11 AMP DECAY (gate-held VCA decay)
// The per-voice state is a few hundred bytes: well under the 16384 B a voice
// has on the Teensy (its own 16 KB engine scratch).
#include <math.h>
#include "rmp/rmp_api.h"

typedef struct {
  /* parameters (scaled) */
  float tune, wave, cut, reso_k, envmod, fdec_k, fdec_acc_k, accamt, level, slide_k, drive_in, drive_out, adec_k;
  /* pitch */
  float note_cur, note_tgt, phase, inc;
  /* ladder */
  float s1, s2, s3, s4;
  /* envelopes */
  float fenv, atk, aenv, acc, acc_lag, vel_gain;
  int32_t gate, sliding, active;
} acid_state;

static const rmp_host *g_host;
static float g_fs = 48000.0f;

static inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   /* NaN -> lo */
static inline float n01(float p) { return clampf(p * (1.0f / 127.0f), 0.0f, 1.0f); }
static inline float fast_tanh(float x) {   /* Pade-style, exact +-1 at |x| = 3 */
  x = clampf(x, -3.0f, 3.0f);
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
static inline float tan_pw(float x) {   /* tan(x) for 0..1.4 (filter prewarp), Pade, rel err < 3e-5 */
  float x2 = x * x;
  return x * (945.0f - 105.0f * x2 + x2 * x2) / (945.0f - 420.0f * x2 + 15.0f * x2 * x2);
}
static inline float poly_blep(float t, float dt, float idt) {   /* idt = 1/dt (no division per sample) */
  if (t < dt) { t *= idt; return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) * idt; return t * t + t + t + 1.0f; }
  return 0.0f;
}
/* the per-sample multiplier for an exponential decay to -60 dB in secs */
static inline float decay_k(float secs) { return expf(-6.9078f / (secs * g_fs)); }

static const float k_defaults[RMP_PARAMS] = {64, 0, 42, 92, 72, 52, 80, 100, 40, 30, 110, 64, 64, 64, 64, 64};

static int32_t acid_init(const rmp_host *h) {
  g_host = h;
  g_fs = h->sample_rate > 1000.0f ? h->sample_rate : 48000.0f;
  return 0;
}

static void acid_params(void *st, const float *p) {
  acid_state *s = st;
  s->tune = (n01(p[0]) - 0.5039f) * 24.0f;
  s->wave = n01(p[1]);
  s->cut = n01(p[2]);
  float r = n01(p[3]);
  s->reso_k = 4.2f * r;                                    /* ~4 = self oscillation */
  s->envmod = 7.0f * n01(p[4]);                            /* octaves at the envelope's peak */
  float d = n01(p[5]);
  s->fdec_k = decay_k(0.08f + 2.4f * d * d);
  s->fdec_acc_k = decay_k(0.12f + 0.25f * d);              /* an accented note decays fast, as on the original */
  s->accamt = n01(p[6]);
  float lv = n01(p[7]);
  s->level = lv * lv * 0.9f;
  float sl = n01(p[8]);
  float sl_secs = 0.008f + 0.35f * sl * sl;                /* glide time constant */
  s->slide_k = 1.0f - expf(-(float)g_host->max_block / (sl_secs * g_fs));   /* per block */
  float dr = n01(p[9]);
  s->drive_in = 0.6f + 1.8f * dr;
  s->drive_out = 1.0f + 5.0f * dr * dr;
  float ad = n01(p[10]);
  s->adec_k = decay_k(0.15f + 6.0f * ad * ad);
}

static void acid_voice_init(void *st) {
  acid_state *s = st;
  acid_params(s, k_defaults);
  s->note_cur = s->note_tgt = 48.0f;
}

static void acid_note_on(void *st, float note, float vel) {
  acid_state *s = st;
  note = clampf(note, 0.0f, 127.0f);
  vel = clampf(vel, 0.0f, 1.0f);
  int accent = vel > 0.8f;
  s->acc = accent ? s->accamt : 0.0f;
  s->vel_gain = 0.75f + 0.25f * vel;
  if (s->gate && s->active) {         /* legato: slide, no retrigger */
    s->note_tgt = note;
    s->sliding = 1;
  } else {
    s->note_cur = s->note_tgt = note;
    s->sliding = 0;
    s->fenv = 1.0f;
    s->atk = 0.0f;
    s->aenv = 1.0f;
    s->active = 1;
  }
  s->gate = 1;
}

static void acid_note_off(void *st) { ((acid_state *)st)->gate = 0; }

static void acid_render(void *st, float *out, int32_t n) {
  acid_state *s = st;
  if (!s->active) { for (int32_t i = 0; i < n; ++i) out[i] = 0.0f; return; }
  /* per block: pitch (slide), cutoff, filter coefficients */
  if (s->sliding) s->note_cur += (s->note_tgt - s->note_cur) * s->slide_k;
  float f = 440.0f * expf((s->note_cur + s->tune - 69.0f) * 0.05776227f);
  float dt = clampf(f / g_fs, 1e-5f, 0.45f), idt = 1.0f / dt;
  /* accent sweep: a lag on accent x filter envelope, so repeated accents build up */
  s->acc_lag += (s->acc * s->fenv - s->acc_lag) * 0.12f;   /* ~8 ms */
  float oct = s->envmod * s->fenv * (1.0f + 0.3f * s->acc) + 1.5f * s->acc_lag;   /* accent: ~+2 octaves at the peak */
  float fc = 22.0f * expf((9.3f * s->cut + oct) * 0.6931472f);
  fc = clampf(fc, 20.0f, 0.42f * g_fs);
  float g = tan_pw(3.14159265f * fc / g_fs);
  float G = g / (1.0f + g), b = 1.0f / (1.0f + g);
  float G2 = G * G, G3 = G2 * G, G4 = G2 * G2;
  float k = s->reso_k;
  float inv = 1.0f / (1.0f + k * G4);
  float comp = 1.0f + 0.55f * k;                           /* make up the passband the resonance takes */
  float fk = s->acc > 0.0f ? s->fdec_acc_k : s->fdec_k;
  float ak = s->gate ? s->adec_k : 0.9985f;                /* gate off: ~100 ms release */
  float gain = s->level * s->vel_gain * (1.0f + 0.5f * s->acc);
  float w = s->wave;
  float norm = 1.0f / fast_tanh(0.8f * s->drive_out);      /* drive changes the colour more than the level */
  float s1 = s->s1, s2 = s->s2, s3 = s->s3, s4 = s->s4, ph = s->phase;
  float fenv = s->fenv, atk = s->atk, aenv = s->aenv;
  for (int32_t i = 0; i < n; ++i) {
    /* oscillator */
    float saw = 2.0f * ph - 1.0f - poly_blep(ph, dt, idt);
    float p2 = ph + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
    float sq = (ph < 0.5f ? 1.0f : -1.0f) + poly_blep(ph, dt, idt) - poly_blep(p2, dt, idt);
    float x = (saw + w * (0.85f * sq - saw)) * s->drive_in;
    ph += dt; if (ph >= 1.0f) ph -= 1.0f;
    /* ladder: solve y4 with the feedback, saturate the input, run the four stages */
    float S = G3 * b * s1 + G2 * b * s2 + G * b * s3 + b * s4;
    float y4 = (G4 * x + S) * inv;
    float u = fast_tanh(x - k * y4);
    float v = (u - s1) * G; float y1 = v + s1; s1 = y1 + v;
    v = (y1 - s2) * G; float y2 = v + s2; s2 = y2 + v;
    v = (y2 - s3) * G; float y3 = v + s3; s3 = y3 + v;
    v = (y3 - s4) * G; y4 = v + s4; s4 = y4 + v;
    /* envelopes */
    atk += (1.0f - atk) * 0.012f;                          /* ~2 ms attack */
    aenv *= ak;
    fenv *= fk;
    float y = fast_tanh(y4 * comp * s->drive_out) * norm;
    y *= gain * atk * aenv;
    if (!(y > -2.0f && y < 2.0f)) { y = 0.0f; s1 = s2 = s3 = s4 = 0.0f; }   /* never NaN */
    out[i] = clampf(y, -1.0f, 1.0f);
  }
  if (fenv < 1e-6f) fenv = 0.0f;
  if (aenv < 1e-5f) { aenv = 0.0f; if (!s->gate) { s->active = 0; s1 = s2 = s3 = s4 = 0.0f; } }
  s->s1 = s1; s->s2 = s2; s->s3 = s3; s->s4 = s4; s->phase = ph;
  s->fenv = fenv; s->atk = atk; s->aenv = aenv;
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 2u, sizeof(acid_state), 40,   /* estimated, not measured: ~130-160 cycles a sample on the M7;
     a Mac timing against sine_test (17) agrees within the guard's 2x. Check peak_permille in device-info */
  acid_init, acid_voice_init, acid_note_on, acid_note_off, acid_params, acid_render,
  {0, 0, 0, 0, 0, 0, 0, 0}};
