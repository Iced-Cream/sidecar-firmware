// plugins/modele/modele.c - MODEL E: a playable three-oscillator voice in the classic three-oscillator layout, one voice a state: a
// mono track is one instrument (glide between its notes), a poly track plays chords on instances (manifest max_voices).
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 trigcondition and contributors. This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version. It is distributed WITHOUT ANY WARRANTY; see LICENSE.
//
// The oscillators, modulation and mixer are ModelDigi's (third_party/modeldigi, unchanged; NOTICE has the source and
// commit): 48 kHz mono Q31 at a fixed 1/16 bank gain, 6 samples late, one modeldigi_osc_render a block of at most 12
// samples, so pitch, levels and modulation move every 0.25 ms. The tuning laws below follow its Digitakt wrapper
// (machines/digitakt/controls.c): the FRQ curve (+-1 semitone fine, then semitones to +-7, x 5 when oscillator 3 is
// free), the ranges with LO's calibrated ratio, the free LO table, the modulation depth table, the squared levels.
// Written here for the Sidecar (the other sections of such a voice, which ModelDigi leaves out):
//   filter    a 4-pole ladder solved without a unit delay (four trapezoidal one-poles, prewarped: the resonance and
//             the self-oscillation sit on the cutoff at any frequency, the loop gain reaches 1 at k = 4), the
//             differential pair at its input as a soft clip; the coefficients at the block's ends, linear between
//   contours  a filter and a loudness contour sharing attack and decay (the SUS knob sets both sustains); DEC's
//             release as the Model D's DECAY switch: on the release is the decay time, off about 6 ms
//   glide     an exponential slew of the keyboard pitch, as hush1: a note glides from this state's last note
//   noise     one-pole coloured white noise beside the bank (the mixer's slot for it)
// The note's stealing as hush1: a note_on on a sounding state keeps its phases and its filter and the contours rise
// from where they are (no click). A silent state renders nothing (its phases rest). No allocation; everything render
// writes is in the state; NaN guarded; the output clamped to +-1.
//   P1 OSC1  P2 OSC2  P3 OSC3 (range + wave, all 36 pairs each)  P4 TUN2  P5 TUN3
//   P6 MIX (osc 1 and 2, both full at the centre)  P7 O3NZ (left noise, right osc 3)  P8 MOD (right: osc 3 follows the
//   keys; left: CONTROL off, osc 3 free)
//   P9 CUT  P10 EMPH  P11 CONT  P12 G<>A (left glide, right attack)  P13 DEC  P14 SUS  P15 MODE  P16 LEV
#include <math.h>
#include "rmp/rmp_api.h"
#include "dsp/oscillator.h"

#define CHUNK 12            /* samples between coefficient updates: the host's block */
#define IN_SCALE (16.0f / 2147483648.0f)
#define LADDER_DRIVE 0.5f   /* the bank's unit at the ladder's input: a saw near the knee, three past it */

enum { ENV_OFF, ENV_ATK, ENV_DEC, ENV_SUS, ENV_REL };
enum { W_TRI, W_TS, W_RSAW, W_SAW, W_SQR, W_WIDE, W_NARR };
enum { R_LO, R_32, R_16, R_8, R_4, R_2 };

/* OSC1..3: all 36 range + wave pairs on one knob (the manifest's lists, in this order): range LO 32 16 8 4 2 x the six waves.
   zone = value x 36 / 128, as the Rytm's list. A Rytm that keeps 16 names gets every 2.25th of them from the host: the same zones. */
static const uint8_t k_wave12[6] = {W_TRI, W_TS, W_SAW, W_SQR, W_WIDE, W_NARR};
static const uint8_t k_wave3[6] = {W_TRI, W_RSAW, W_SAW, W_SQR, W_WIDE, W_NARR};

/* the ranges in octaves from 8'; LO is ModelDigi's calibrated LO / audio ratio (-459156 / 65536) */
static const float k_range[6] = {-7.00616455f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f};
/* CONTROL off at LO: log2 Hz at tune amounts 0, .25, .5, .75, 1 (ModelDigi's frozen table) */
static const float k_lo_log2[5] = {-3.86798096f, -2.44403076f, -0.96537781f, 0.48719788f, 1.94714355f};
/* the modulation knob's amount to calibrated octaves (ModelDigi's table) */
static const float k_dep_amt[7] = {0.0f, 0.05000305f, 0.10000610f, 0.25f, 0.5f, 0.75f, 1.0f};
static const float k_dep_oct[7] = {0.0f, 0.05700684f, 0.11331177f, 0.28219604f, 0.44303894f, 0.75479126f, 1.50897217f};
/* MODE: keyboard tracking of the cutoff per entry group of four */
static const float k_trk[4] = {0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f};

typedef struct {
  modeldigi_osc_state bank;
  /* parameters (scaled) */
  uint8_t range[3], wave[3];
  float t2, t3, l1, l2, l3, noise, depth, cut, res, cont, trk, a_k, d_k, r_k, fsus, asus, glide_s, level;
  int32_t control, analog, dirty;
  /* smoothed per block (~5 ms) */
  float sm_t2, sm_t3, sm_l1, sm_l2, sm_l3, sm_noise, sm_depth, sm_cut, sm_res, sm_level;
  /* the note: the slewed pitch and its target, semitones */
  float pitch, target;
  int32_t has_note;
  /* the bank's parameters as last computed */
  modeldigi_osc_params op;
  /* the ladder: four integrators, the coefficients the last block ended on; noise's colour */
  float s1, s2, s3, s4, G, inv, nlp;
  /* the contours */
  float aenv, fenv;
  int32_t astage, fstage;
  uint32_t rng;
} md_state;

static float g_fs = 48000.0f;
static float g_sk = 0.0f;   /* the knob smoothing per CHUNK samples */

static inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   /* NaN -> lo */
static inline float n01(float p) { return clampf(p * (1.0f / 127.0f), 0.0f, 1.0f); }
static inline int32_t band16(float p) { return ((int32_t)clampf(p, 0.0f, 127.0f) * 16) >> 7; }
static inline int32_t band36(float p) { return ((int32_t)clampf(p, 0.0f, 127.0f) * 36) >> 7; }
static inline float pow2f(float oct) { return expf(oct * 0.69314718f); }
static inline float tan_pw(float x) {   /* tan(x) for 0..1.4 (filter prewarp), Pade, rel err < 3e-5 */
  float x2 = x * x;
  return x * (945.0f - 105.0f * x2 + x2 * x2) / (945.0f - 420.0f * x2 + 15.0f * x2 * x2);
}
static inline float sat(float x) {      /* the pair's tanh as a cubic: slope 1 at 0, +-1 with slope 0 at +-1.5 */
  x = clampf(x, -1.5f, 1.5f);
  return x - x * x * x * (1.0f / 6.75f);
}
static inline uint32_t xs32(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static inline float rnd_pm(uint32_t *s) { return (float)(int32_t)xs32(s) * (1.0f / 2147483648.0f); }   /* -1..1 */

/* a centred knob (64 the centre, as a Rytm bipolar knob) as -1 .. 1 */
static inline float centred(float p) {
  float x = clampf(p, 0.0f, 127.0f) - 64.0f;
  return x < 0.0f ? x * (1.0f / 64.0f) : x * (1.0f / 63.0f);
}
/* ModelDigi's FRQ curve in semitones: quadratic to 1 semitone at 21 steps from the centre, then 1 semitone per 7 */
static float frq_semis(float p) {
  float x = clampf(p, 0.0f, 127.0f) - 64.0f, a = fabsf(x);
  if (a > 63.0f) a = 63.0f;
  float c = a <= 21.0f ? (a / 21.0f) * (a / 21.0f) : 1.0f + (a - 21.0f) / 7.0f;
  return x < 0.0f ? -c : c;
}
/* 0..1 -> 1 ms .. 10 s, exponential; to e^-4.6 (-40 dB) in t, as hush1 */
static inline float p_secs(float p) { return 0.001f * expf(9.2103404f * p); }
static inline float rc_k(float secs) { return expf(-4.6f / (secs * g_fs)); }

static float depth_of(float a) {
  int32_t k = 1;
  while (k < 6 && a > k_dep_amt[k]) ++k;
  return k_dep_oct[k - 1] + (k_dep_oct[k] - k_dep_oct[k - 1]) * (a - k_dep_amt[k - 1]) / (k_dep_amt[k] - k_dep_amt[k - 1]);
}

static const float k_defaults[RMP_PARAMS] = {72, 72, 56, 70, 64, 64, 104, 64, 44, 44, 72, 64, 66, 64, 20, 100};

static int32_t md_init(const rmp_host *h) {
  g_fs = h->sample_rate > 1000.0f ? h->sample_rate : 48000.0f;
  g_sk = 1.0f - expf(-(float)CHUNK / (0.005f * g_fs));
  return 0;
}

static void md_params(void *st, const float *p) {
  md_state *s = st;
  for (int32_t v = 0; v < 3; ++v) {
    int32_t z = band36(p[v]);
    s->range[v] = (uint8_t)(z / 6);
    s->wave[v] = (v == 2 ? k_wave3 : k_wave12)[z % 6];
  }
  s->t2 = frq_semis(p[3]);
  s->t3 = frq_semis(p[4]);
  /* osc 1 and 2 full at the centre: one falls away towards its end, the other too in the last 10 % (0 and 127: neither) */
  float mix = centred(p[5]), m1 = fabsf(mix) * (1.0f / 0.9f), rest = m1 > 1.0f ? 10.0f * (1.0f - fabsf(mix)) : 1.0f;
  float fall = m1 < 1.0f ? 1.0f - m1 : 0.0f;
  float a1 = mix > 0.0f ? fall : rest, a2 = mix > 0.0f ? rest : fall;
  if (mix == 0.0f) a1 = a2 = 1.0f;
  s->l1 = a1 * a1;
  s->l2 = a2 * a2;
  float o3nz = centred(p[6]);                               /* right osc 3 (its level, it modulates at any level) */
  s->l3 = o3nz > 0.0f ? o3nz * o3nz : 0.0f;
  s->noise = o3nz < 0.0f ? o3nz * o3nz : 0.0f;
  float m = centred(p[7]);                                  /* the sign is the OSC 3 CONTROL switch */
  s->control = m >= 0.0f;
  s->depth = depth_of(fabsf(m));
  s->cut = n01(p[8]);
  s->res = n01(p[9]) * 5.0f;                                /* the loop gain reaches 1 at k = 4: EMPH ~102 */
  s->cont = n01(p[10]);
  float ga = centred(p[11]);                                /* left glide, right attack */
  s->glide_s = ga < 0.0f ? ga * ga * 1.5f : 0.0f;
  /* the attack aims at 1.3 and turns at 1.0: 0 -> 1 in t takes t / ln(1.3 / 0.3) a time constant */
  s->a_k = 1.0f - expf(-1.4663371f / ((ga > 0.0f ? p_secs(ga) + 0.0005f : 0.0015f) * g_fs));
  float dec = p_secs(n01(p[12])) + 0.003f;
  s->d_k = rc_k(dec);
  float sus = n01(p[13]);                                   /* 0 a pluck, the centre full loudness, the top an organ */
  s->asus = sus < 0.5f ? 2.0f * sus : 1.0f;
  s->fsus = sus > 0.5f ? 2.0f * sus - 1.0f : 0.0f;
  int32_t mode = band16(p[14]);
  s->trk = k_trk[mode & 3];
  s->r_k = (mode & 4) ? s->d_k : rc_k(0.006f);              /* the DECAY switch */
  s->analog = !(mode & 8);
  float lv = n01(p[15]);
  s->level = lv * lv * 1.25f;
  s->dirty = 1;
}

static void md_voice_init(void *st) {
  md_state *s = st;
  modeldigi_osc_init(&s->bank);
  s->rng = 0x7E57AB1Eu ^ (uint32_t)(uintptr_t)st;
  if (!s->rng) s->rng = 1u;
  s->pitch = s->target = 60.0f;
  md_params(s, k_defaults);
  s->sm_t2 = s->t2; s->sm_t3 = s->t3; s->sm_l1 = s->l1; s->sm_l2 = s->l2; s->sm_l3 = s->l3; s->sm_noise = s->noise;
  s->sm_depth = s->depth; s->sm_cut = s->cut; s->sm_res = s->res; s->sm_level = s->level;
}

static void md_note_on(void *st, float note, float vel) {
  md_state *s = st;
  (void)vel;                                                /* the Model D has no velocity */
  note = clampf(note, 0.0f, 127.0f);
  if (s->astage == ENV_OFF) {                               /* a fresh note: a clean ladder, a breath for self-oscillation */
    s->s1 = 2e-3f; s->s2 = s->s3 = s->s4 = 0.0f;
    s->nlp = 0.0f;
    s->aenv = s->fenv = 0.0f;
    s->G = -1.0f;                                           /* no block to interpolate from */
  }
  s->target = note;
  if (!s->has_note || s->glide_s <= 0.0f) s->pitch = note;  /* else it glides from the last note */
  s->has_note = 1;
  s->astage = s->fstage = ENV_ATK;                          /* the contours rise from where they are */
  s->dirty = 1;
}

static void md_note_off(void *st) {
  md_state *s = st;
  if (s->astage != ENV_OFF) s->astage = ENV_REL;
  if (s->fstage != ENV_OFF) s->fstage = ENV_REL;
}

static inline float env_step(float e, int32_t *stage, float a_k, float d_k, float sus, float r_k) {
  switch (*stage) {
    case ENV_ATK: e += (1.3f - e) * a_k; if (e >= 1.0f) { e = 1.0f; *stage = ENV_DEC; } break;
    case ENV_DEC: e = sus + (e - sus) * d_k; if (e <= sus + 1e-4f) { e = sus; *stage = ENV_SUS; } break;
    case ENV_SUS: e += (sus - e) * 0.002f; break;           /* SUS moves under a held note without a step */
    case ENV_REL: e *= r_k; if (e < 1e-4f) { e = 0.0f; *stage = ENV_OFF; } break;
    default: e = 0.0f; break;
  }
  return e;
}

/* Hz x 65536 for the bank, within its bounds (a float is exact to 0.002 Hz at the top: well under a cent) */
static inline uint32_t q16(float hz) { return (uint32_t)(clampf(hz, 0.0625f, 20000.0f) * 65536.0f); }

/* the three pitches: keyboard (with glide), range, tuning; osc 3 free of the keyboard with CONTROL off */
static void pitches(md_state *s) {
  float keyed = (s->pitch - 60.0f) * (1.0f / 12.0f);
  float c4 = 261.6256f;
  s->op.frequency_q16_16[0] = q16(c4 * pow2f(k_range[s->range[0]] + keyed));
  s->op.frequency_q16_16[1] = q16(c4 * pow2f(k_range[s->range[1]] + keyed + s->sm_t2 * (1.0f / 12.0f)));
  float f3;
  if (s->control) {
    f3 = c4 * pow2f(k_range[s->range[2]] + keyed + s->sm_t3 * (1.0f / 12.0f));
  } else if (s->range[2] == R_LO) {                         /* five times the sweep, the calibrated free LO rates */
    float pos = clampf((s->sm_t3 * 5.0f + 35.025f) * (4.0f / 70.05f), 0.0f, 4.0f);
    int32_t i = pos >= 3.0f ? 3 : (int32_t)pos;
    f3 = pow2f(k_lo_log2[i] + (k_lo_log2[i + 1] - k_lo_log2[i]) * (pos - (float)i));
  } else {                                                  /* the audio ranges from C2 */
    f3 = c4 * pow2f(k_range[s->range[2]] - 2.0f + s->sm_t3 * 5.0f * (1.0f / 12.0f));
  }
  s->op.frequency_q16_16[2] = q16(f3);
}

/* one block of at most CHUNK samples */
static void render_chunk(md_state *s, float *out, int32_t n) {
  float sk = n == CHUNK ? g_sk : g_sk * (float)n * (1.0f / (float)CHUNK);
  s->sm_cut += (s->cut - s->sm_cut) * sk;
  s->sm_res += (s->res - s->sm_res) * sk;
  s->sm_level += (s->level - s->sm_level) * sk;
  s->sm_l1 += (s->l1 - s->sm_l1) * sk;
  s->sm_l2 += (s->l2 - s->sm_l2) * sk;
  s->sm_l3 += (s->l3 - s->sm_l3) * sk;
  s->sm_noise += (s->noise - s->sm_noise) * sk;
  s->sm_depth += (s->depth - s->sm_depth) * sk;
  int32_t move = s->dirty;
  if (s->sm_t2 != s->t2) {
    s->sm_t2 += (s->t2 - s->sm_t2) * sk; move = 1;
    if (fabsf(s->t2 - s->sm_t2) < 1e-3f) s->sm_t2 = s->t2;
  }
  if (s->sm_t3 != s->t3) {
    s->sm_t3 += (s->t3 - s->sm_t3) * sk; move = 1;
    if (fabsf(s->t3 - s->sm_t3) < 1e-3f) s->sm_t3 = s->t3;
  }
  if (s->pitch != s->target) {
    s->pitch += (s->target - s->pitch) * (1.0f - expf(-(float)n / (s->glide_s * g_fs + 1.0f)));
    if (fabsf(s->target - s->pitch) < 1e-3f) s->pitch = s->target;
    move = 1;
  }
  if (move) { pitches(s); s->dirty = 0; }

  /* the bank */
  modeldigi_osc_params *op = &s->op;
  for (int32_t v = 0; v < 3; ++v) op->waveform[v] = s->wave[v];
  op->level[0] = (uint16_t)(s->sm_l1 * 65535.0f + 0.5f);
  op->level[1] = (uint16_t)(s->sm_l2 * 65535.0f + 0.5f);
  op->level[2] = (uint16_t)(s->sm_l3 * 65535.0f + 0.5f);
  op->modulation_depth_q16_16 = (uint32_t)(s->sm_depth * 65536.0f);
  op->modulation_enabled = op->modulation_depth_q16_16 != 0;
  op->osc3_control = s->control != 0;
  op->analog = s->analog != 0;
  int32_t bank[CHUNK];
  modeldigi_osc_render(&s->bank, op, bank, (size_t)n);

  /* the contours, sample by sample; the filter's at the block's end */
  float a_k = s->a_k, d_k = s->d_k, r_k = s->r_k;
  float aenv = s->aenv, fenv = s->fenv;
  for (int32_t i = 0; i < n; ++i) {
    aenv = env_step(aenv, &s->astage, a_k, d_k, s->asus, r_k);
    fenv = env_step(fenv, &s->fstage, a_k, d_k, s->fsus, r_k);
    out[i] = aenv;                                          /* the loudness curve, for the ladder's loop below */
  }
  s->aenv = aenv;
  s->fenv = fenv;
  if (s->astage == ENV_SUS && s->asus <= 0.0f) s->astage = ENV_OFF;   /* a pluck that has died under a held gate */

  /* the cutoff: 20 Hz .. 20 kHz over the knob, 7 octaves of contour, the keys from C4 */
  float oct = 10.0f * s->sm_cut + 7.0f * s->cont * fenv + s->trk * (s->pitch - 60.0f) * (1.0f / 12.0f);
  float fc = clampf(20.0f * pow2f(oct), 8.0f, 20500.0f);
  float g = tan_pw(3.14159265f * fc / g_fs);
  float Gn = g / (1.0f + g);
  float k = s->sm_res, G2n = Gn * Gn;
  float invn = 1.0f / (1.0f + k * G2n * G2n);
  float G = s->G < 0.0f ? Gn : s->G, inv = s->G < 0.0f ? invn : s->inv;
  float rate = 1.0f / (float)n, dG = (Gn - G) * rate, dinv = (invn - inv) * rate;
  float s1 = s->s1, s2 = s->s2, s3 = s->s3, s4 = s->s4, nlp = s->nlp;
  float gn = s->sm_noise * 1.8f;
  float gain = s->sm_level * (1.0f + 0.45f * k);            /* the ladder's passband loss, 1 / (1 + k), partly back */
  uint32_t rng = s->rng;
  for (int32_t i = 0; i < n; ++i) {
    float x = (float)bank[i] * IN_SCALE + 1e-12f;           /* (the offset keeps the integrators out of denormals) */
    if (gn > 0.0f) {
      float white = rnd_pm(&rng);
      nlp += 0.11f * (white - nlp);
      x += gn * (0.8f * nlp + 0.2f * white);
    }
    x *= LADDER_DRIVE;
    G += dG; inv += dinv;
    float b = 1.0f - G;
    float bs1 = b * s1, bs2 = b * s2, bs3 = b * s3, bs4 = b * s4;
    float S = ((bs1 * G + bs2) * G + bs3) * G + bs4;        /* y4 = G^4 u + S: the loop solved for u */
    float u = sat((x - k * S) * inv);
    float v = (u - s1) * G, y = v + s1; s1 = y + v;
    v = (y - s2) * G; y = v + s2; s2 = y + v;
    v = (y - s3) * G; y = v + s3; s3 = y + v;
    v = (y - s4) * G; y = v + s4; s4 = y + v;
    float o = y * out[i] * gain;
    if (!(y > -8.0f && y < 8.0f)) { o = 0.0f; s1 = s2 = s3 = s4 = nlp = 0.0f; }   /* never NaN, never runaway */
    out[i] = clampf(o, -1.0f, 1.0f);
  }
  s->s1 = s1; s->s2 = s2; s->s3 = s3; s->s4 = s4; s->nlp = nlp; s->G = G; s->inv = inv; s->rng = rng;
  if (s->astage == ENV_OFF) { s->fstage = ENV_OFF; s->aenv = s->fenv = 0.0f; s->s1 = s->s2 = s->s3 = s->s4 = 0.0f; }
}

static void md_render(void *st, float *out, int32_t n) {
  md_state *s = st;
  for (int32_t at = 0; at < n; at += CHUNK) {
    int32_t m = n - at < CHUNK ? n - at : CHUNK;
    if (s->astage == ENV_OFF) { for (int32_t i = 0; i < m; ++i) out[at + i] = 0.0f; continue; }
    render_chunk(s, out + at, m);
  }
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 2u, sizeof(md_state), MODELE_PERMILLE,   /* MODELE_PERMILLE: the Makefile's, from tests/md_check */
  md_init, md_voice_init, md_note_on, md_note_off, md_params, md_render,
  {0, 0, 0, 0, 0, 0, 0, 0}};
