// plugins/bp303/bp303.c - BP303: a 303-style bass line voice, the BLACKPYRAMIDS_303 voice in float.
// SPDX-License-Identifier: MIT
// The voice is BLACKPYRAMIDS_303's (Zac Kyoti and the OT Kyoti FW contributors, MIT: LICENSE.kyoti), a recalibration
// of Open303 (Robin Schmidt, 2009, MIT: LICENSE.open303) on the TB-303's circuit. Ported from that project's float /
// C++ reference of the voice (its integer twin and the knob-to-word code: NOTICE has the files and the commit), not
// from its DSP assembly: the same signal path and values, in floating point, at the host's sample rate.
// Adapted for the Sidecar (Copyright 2026 trigcondition and contributors, MIT).
//
//   saw (polyBLEP) -+- blend (WAVE) - input coupling 0.7 Hz - ladder (4x) --- half-band 4x>2x (15 taps)
//   square (tanh)  -+      resonance feedback - high-pass 55 Hz -+        - half-band 2x>1x (43 taps)
//     - allpass 14 Hz - output coupling 110 Hz - notch - VCA (amp envelope, de-clicker) - 16 kHz rounding
//     - soft drive (VOL 65..127)
//   cutoff = base (CUT, ENV's offset) + ENV x filter envelope + accent RC + the accent sweep capacitor
//
// Open303's: the oscillator blend and square shaper, the TeeBee ladder (TB_303 mode) and its fitted b0 / k / g
// formulas, the envelopes, accent and slide, the post filters' structure, the cutoff / env mod mapping.
// BLACKPYRAMIDS_303's: the couplings (0.7 / 55 / 110 Hz), the resonance curve by cutoff and the ring term, make-up
// x 1.3, the 70 Hz filter floor, the accent RC (8.9 ms) and decay (tau 68 ms), the accent sweep capacitor on the RES
// and ACCENT pots, the VCA decay (tau 1.35 s), the level by cutoff, the soft attack (1 ms), the 2 ms release, the
// drive on VOL, the 1 ms cutoff / env glide and the eased resonance, the half-band decimators, every knob law.
// The Sidecar's: float instead of 24-bit fixed point (no word saturations inside the voice; the clean output clips
// at +-1 as the original's does), every time constant made for the host's rate (48 kHz, the filter at 192 kHz; the
// original runs 44.1 / 176.4 kHz), a 16-sample control step with its eases scaled to that rate, knob values with
// their fraction (the tables interpolated), notes as the Sidecar sends them:
//   accent  a note with velocity above 100 (of 127) is accented, by the ACC knob's amount
//   slide   a note while the gate is still open (an overlapping trig) glides there (SLID) with no retrigger
//   gate    the track's own note length: the gate closes at note_off (the original's GATE knob is not here)
//
//   P1 CUT  P2 RES  P3 ENV  P4 DEC  P5 ACC  P6 WAVE  P7 SLID  P8 VOL  P9 TUNE
#include <math.h>
#include "rmp/rmp_api.h"
#include "bp303_tables.h"

#define LT_BASE 70.0f                /* the cutoff table's floor, Hz */
#define LT_SPO 26                    /* entries an octave */
#define LT_N 216                     /* 70 Hz .. 20 kHz is 212.1 steps */
#define CTL 16                       /* the control step, samples (the original's frame) */
#define HB4_N 15
#define HB2_N 43
#define ACCENT_VEL (100.5f / 127.0f)
#define K8_MULT 0.8333333f           /* the accent VCA amount */
#define RUN_ON_S 2.0f                 /* the filter runs on this long behind a closed VCA, then the voice sleeps */

/* the half-bands' odd taps, one side (they are symmetric): 4x > 2x (15 taps), 2x > 1x (43 taps); centre 0.5 */
static const float k_hb4[4] = {-0.0025498822324204265f, 0.017210679383708637f, -0.06801648574455314f,
                               0.30333545754425784f};
static const float k_hb2[11] = {6.212766482125498e-05f, -0.0002875745709729691f, 0.0008894486688395776f,
                                -0.002209353335128785f, 0.004752980633200525f, -0.009239059622939262f,
                                0.016736870523245965f, -0.02912813043538308f, 0.05085289646098107f,
                                -0.09791883984456011f, 0.3154936715325458f};

enum { K_CUT, K_RES, K_ENV, K_DEC, K_ACC, K_WAVE, K_SLID, K_VOL, K_TUNE, K_N };   /* P1 .. P9 */

typedef struct {
  /* the knobs, 0..127 with their fraction */
  float k[K_N];
  int32_t dirty;
  /* what the knobs make (control step): the targets */
  float cs, l0, es, rs_t, ag_t, bg_t, sc_t, gd_t, w1, w2, w3, respot;
  int32_t sq, have;
  /* eased: the resonance words and the level; the clean / driven hand-over */
  float rs, ag, bg, sc, split, sco, sc_out, gd_out;
  int32_t drv;
  /* the cutoff and env amount, glided per sample toward l0t / est */
  float l0g, esg, l0t, est;
  /* the accent sweep capacitor */
  float wmeg, wcap, wamt;
  /* the note */
  float c_meg, agn, k8, m_amp, m_off, note;
  int32_t agf, gate, active, ctl, quiet;                 /* quiet: samples since the VCA closed */
  double ph, inc, inc_t;
  /* envelopes: main (filter), its soft attack, the accent RC, the VCA; the de-clicker's states */
  float meg, megs, rc2, amp, d1, d2;
  /* the 4x loop */
  float hpx, hpy, fbx, fby, y1, y2, y3, y4;
  float hb4[2 * HB4_N], hb2[2 * HB2_N];
  int32_t hb4p, hb2p;
  /* after the decimators */
  float apx, apy, h2x, h2y, tops;
  double n1, n2;
} bp_state;

/* made once for the host's rate (init) */
static float g_fs = 48000.0f;
static float g_b0[LT_N + 1], g_kp[LT_N + 1], g_th[514], g_lmax;
static float g_hp1a, g_hp1b, g_fba, g_fbb, g_hp2a, g_hp2b, g_apc, g_top, g_da1, g_da2, g_da3;
static double g_na1, g_na2, g_na3, g_nk;
static float g_gld, g_rc1, g_c2, g_macc, g_amp_on, g_off_n, g_off_a;
static float g_ease, g_split, g_wdecay, g_tc_ms;

static inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   /* NaN -> lo */
static inline float knob(float p) { return clampf(p, 0.0f, 127.0f); }
/* a 128-entry knob table at a value with a fraction, / scale */
#define KLERP(tab, k, scale) klerp_((k), (float)(tab)[(int32_t)(k)], (float)(tab)[(int32_t)(k) < 127 ? (int32_t)(k) + 1 : 127], (scale))
static inline float klerp_(float k, float a, float b, float scale) { return (a + (b - a) * (k - (float)(int32_t)k)) * scale; }
static inline float th_lookup(float x) {                    /* tanh over [-8, 8), 1/32 a step, interpolated */
  float t = (clampf(x, -8.0f, 7.9999f) + 8.0f) * 32.0f;
  int32_t i = (int32_t)t;
  return g_th[i] + (g_th[i + 1] - g_th[i]) * (t - (float)i);
}
/* the saw at phase p (-1 .. 1) with its polyBLEP: the step (+1 > -1) sits at the wrap */
static inline float blep(float p, float inc, float iinc) {
  float da = p + 1.0f, db = 1.0f - p;
  if (da < inc) { float w = (inc - da) * iinc; return p + w * w; }
  if (db <= inc) { float w = (inc - db) * iinc; return p - w * w; }
  return p;
}
static inline float one_pole_a(float hz, float fs) { return expf(-6.2831853f * hz / fs); }
static inline float decay_c(float tau_s) { return expf(-1.0f / (tau_s * g_fs)); }

static const float k_defaults[RMP_PARAMS] = {44, 64, 32, 72, 100, 0, 73, 99, 64, 64, 64, 64, 64, 64, 64, 64};

static int32_t bp_init(const rmp_host *h) {
  g_fs = h->sample_rate > 1000.0f ? h->sample_rate : 48000.0f;
  const float fo = 4.0f * g_fs;
  /* the ladder's coefficients over log2(cutoff): Open303's fitted b0 and k formulas, k x the resonance curve by
     cutoff (0.7 at 200 Hz, 1 at 500 Hz, 1.02 at 1.2 kHz, 1 at 3 kHz; log-f between) */
  static const float kf[4] = {200.0f, 500.0f, 1200.0f, 3000.0f}, km[4] = {0.7f, 1.0f, 1.02f, 1.0f};
  for (int32_t i = 0; i <= LT_N; ++i) {
    double fc = LT_BASE * pow(2.0, (double)i / LT_SPO);
    double fx = fc / (fo * 1.4142135623730951);
    double m = km[3];
    if (fc <= kf[0]) m = km[0];
    else for (int32_t j = 1; j < 4; ++j)
      if (fc <= kf[j]) { m = km[j - 1] + (km[j] - km[j - 1]) * log(fc / kf[j - 1]) / log(kf[j] / kf[j - 1]); break; }
    g_b0[i] = (float)((0.00045522346 + 6.1922189 * fx) / (1.0 + 12.358354 * fx + 4.4156345 * (fx * fx)));
    g_kp[i] = (float)(m * (fx * (fx * (fx * (fx * (fx * (fx + 7198.6997) - 5837.7917) - 476.47308) + 614.95611) + 213.87126) + 16.998792));
  }
  g_lmax = log2f(20000.0f / LT_BASE);
  for (int32_t i = 0; i < 514; ++i) g_th[i] = tanhf(-8.0f + (float)i / 32.0f);
  g_hp1a = one_pole_a(0.7f, fo); g_hp1b = 0.5f * (1.0f + g_hp1a);                 /* the input coupling */
  g_fba = one_pole_a(55.027386f, fo); g_fbb = 0.5f * (1.0f + g_fba);              /* the resonance loop */
  g_hp2a = one_pole_a(110.0f, g_fs); g_hp2b = 0.5f * (1.0f + g_hp2a);             /* the output coupling */
  { float t = tanf(3.14159265f * 14.008f / g_fs); g_apc = (t - 1.0f) / (t + 1.0f); }
  { float k = tanf(3.14159265f * 16000.0f / g_fs); g_top = k / (1.0f + k); }
  {   /* Open303's notch: band-reject 7.5164 Hz, 4.7 octaves, as a TPT state-variable filter */
    double w = 2.0 * 3.141592653589793 * 7.5164 / g_fs, g = tan(0.5 * w);
    g_nk = 2.0 * sinh(0.5 * log(2.0) * 4.7 * w / sin(w));
    g_na1 = 1.0 / (1.0 + g * (g + g_nk)); g_na2 = g * g_na1; g_na3 = g * g_na2;
  }
  {   /* the amp de-clicker: low-pass 200 Hz, Q sqrt(0.5) */
    float g = tanf(3.14159265f * 200.0f / g_fs);
    g_da1 = 1.0f / (1.0f + g * (g + 1.4142136f)); g_da2 = g * g_da1; g_da3 = g * g_da2;
  }
  g_gld = 1.0f - decay_c(0.001f);                          /* the cutoff / env glide, tau 1 ms */
  g_rc1 = decay_c(0.001f);                                 /* the soft attack, 1 ms */
  g_c2 = decay_c(0.008875740f);                            /* the accent RC */
  g_macc = decay_c(0.157f / 2.3025851f);                   /* an accented note's envelope: -20 dB in 157 ms */
  g_amp_on = decay_c(1.35f);                               /* the VCA while the gate is open */
  g_off_n = decay_c(0.002f); g_off_a = decay_c(0.050f);    /* its release: 2 ms, 50 ms after an accent */
  /* the control step: the original's is 16 samples at 44.1 kHz, its eases 1/6 and 1/4 a step */
  float steps = ((float)CTL / g_fs) / (16.0f / 44100.0f);
  g_ease = 1.0f - powf(5.0f / 6.0f, steps);
  g_split = 1.0f - powf(0.75f, steps);
  g_tc_ms = 1000.0f * (float)CTL / g_fs;
  g_wdecay = expf(-2.3025851f * (float)CTL / (0.157f * g_fs));
  return 0;
}

static void bp_params(void *st, const float *p) {
  bp_state *s = st;
  for (int32_t i = 0; i < K_N; ++i) {
    float v = knob(p[i]);
    if (s->k[i] != v) { s->k[i] = v; s->dirty = 1; }
  }
}

/* the knobs -> the continuous words (the original's params.c and record.c, in float) */
static void bp_cont(bp_state *s) {
  /* CUT: the real 303's range (the engine's 1 .. 67) on 0 .. 100, evenly in octaves, then an octave more */
  float q = s->k[K_CUT];
  float cut = q <= 100.0f ? 1.0f + q * 0.66f : 67.0f + (q - 100.0f) * (22.50391f / 27.0f);
  /* RES through its taper; in CUT's extension the span above RES 100 eases toward it (x 0.5 at CUT 127) */
  float rq = KLERP(BP_RES_TAPER_Q8, s->k[K_RES], 1.0f / 256.0f);
  if (q > 100.0f && s->k[K_RES] > 100.0f) {
    float base = (float)BP_RES_TAPER_Q8[100] * (1.0f / 256.0f);
    rq = base + (rq - base) * (1.0f - 0.5f * (q - 100.0f) * (1.0f / 27.0f));
  }
  /* cutoff 200 x 50^f Hz and env mod: Open303's measured mapping (scaler and offset lines) */
  float f = cut * (1.0f / 127.0f), e = s->k[K_ENV] * (1.0f / 127.0f);
  float cc = -0.22168679f + 1.92512439f * f;
  float slo = 3.77399633f * e + 0.73696559f, shi = 4.19454879f * e + 0.86434490f;
  float scl = slo + (shi - slo) * cc, off = 0.04829293f * cc + 0.29439120f;
  s->l0 = 5.64385619f * f - scl * off + 1.51457317f;        /* octaves above the 70 Hz floor: + log2(200 / 70) */
  s->es = scl;
  /* resonance: rs = (1 - e^-3r) / (1 - e^-3), x the ring term by cutoff; the make-up gain's two words */
  float fk = 0.2f * expf(3.91202301f * f);                  /* the base cutoff, kHz */
  float r = rq * (1.0f / 127.0f);
  float rs = (1.0f - expf(-3.0f * r)) * 1.05239570f;
  rs *= 1.0f + 0.02f * r * sqrtf(sqrtf(fk));
  if (rs > 1.0f) rs = 1.0f;
  s->rs_t = rs;
  s->ag_t = 1.0f - rs * rs;
  s->bg_t = rs * (1.0f + rs) * (1.3f / 17.0f);
  if (s->bg_t > 0.125f) s->bg_t = 0.125f;                  /* the original's word is full there (rs above 0.873): kept */
  /* VOL: 0 off, 1 .. 64 clean (-30 dB .. the top), 65 .. 127 the top a quarter down into the soft drive (x 4 and
     0 .. +4.5 dB), with up to 3 dB more level ramped in from 65 to 99; x the trim at the bottom of the resonance,
     the level by cutoff and the headroom factor */
  float vol = s->k[K_VOL], top = 0.87798376f, sc, gd = 0.0f;
  if (vol >= 64.5f) {
    float d = vol - 65.0f; if (d < 0.0f) d = 0.0f;
    sc = 0.25f * top * expf(0.11512925f * 3.0f * (d < 34.0f ? d * (1.0f / 34.0f) : 1.0f));
    gd = 4.0f * expf(0.11512925f * 4.5f * (vol - 64.0f) * (1.0f / 63.0f));
  } else if (vol >= 1.0f) sc = top * expf(-0.11512925f * 30.0f * (64.0f - (vol > 64.0f ? 64.0f : vol)) * (1.0f / 63.0f));
  else sc = vol * top * 0.03162278f;
  float trim = 1.0f;                                       /* (the table ends at 20: none from there) */
  if (rq < 20.0f) {
    int32_t ti = (int32_t)rq;
    trim = ((float)BP_RTRIM_Q14[ti] + (float)(BP_RTRIM_Q14[ti + 1] - BP_RTRIM_Q14[ti]) * (rq - (float)ti)) * (1.0f / 16384.0f);
  }
  s->sc_t = sc * trim * expf(-0.05f * logf(fk)) * 0.93f;
  s->gd_t = gd;
  /* WAVE: osc / 2 = G (-(1 - b) / 2 saw + b / 4 tanh(..) + b dc / 4), G the WAVE gain */
  float b = s->k[K_WAVE] * (1.0f / 127.0f), g = KLERP(BP_WGAIN_Q14, s->k[K_WAVE], 1.0f / 16384.0f);
  s->w1 = -0.5f * (1.0f - b) * g;
  s->w2 = 0.25f * b * g;
  s->w3 = -0.01561065f * b * g;
  s->sq = b > 0.0f;
  /* SLID: the pitch slew, tau 0.3125 ms a knob value (73: the 303's 22.7 ms); 0 jumps */
  s->cs = s->k[K_SLID] < 0.01f ? 1.0f : 1.0f - expf(-1.0f / (0.0003125f * s->k[K_SLID] * g_fs));
  s->respot = KLERP(BP_RES_POT_Q16, s->k[K_RES], 1.0f / 65536.0f);
}

/* one control step: the eases, the clean / driven hand-over, the accent sweep capacitor */
static void bp_control(bp_state *s) {
  if (s->dirty || !s->have) {
    bp_cont(s);
    s->dirty = 0;
    if (!s->have) {
      s->rs = s->rs_t; s->ag = s->ag_t; s->bg = s->bg_t; s->sc = s->sc_t;
      s->have = 1; s->split = 1.0f;
    }
  }
  float d;
  d = s->rs_t - s->rs; s->rs = fabsf(d) < 1e-6f ? s->rs_t : s->rs + d * g_ease;
  d = s->ag_t - s->ag; s->ag = fabsf(d) < 1e-6f ? s->ag_t : s->ag + d * g_ease;
  d = s->bg_t - s->bg; s->bg = fabsf(d) < 1e-6f ? s->bg_t : s->bg + d * g_ease;
  d = s->sc_t - s->sc; s->sc = fabsf(d) < 1e-6f ? s->sc_t : s->sc + d * g_ease;
  /* crossing VOL 64 / 65 the path changes (clean, or the level a quarter into the drive's x 4): the two trade over a
     few steps with their product held, so the output does not jump */
  int32_t want = s->gd_t > 0.0f;
  if (want && s->drv != 1) {
    s->sc = s->sc_t; s->drv = 1;
    s->split = s->sco > 0.0f && s->sc_t > 0.0f ? s->sco / s->sc_t : 1.0f;
  } else if (!want && s->drv == 1) {
    s->sc = s->sc_t; s->drv = 3;
    s->split = s->sco > 0.0f && s->sc_t > 0.0f ? s->sco / s->sc_t : 1.0f;
  }
  if (s->split != 1.0f) {
    d = 1.0f - s->split;
    s->split = fabsf(d) < 0.001f ? 1.0f : s->split + d * g_split;
  }
  if (s->drv == 3 && s->split == 1.0f) s->drv = 0;
  if (s->split == 1.0f) {
    s->sc_out = s->sc; s->gd_out = s->drv == 1 ? s->gd_t : 0.0f;
  } else {
    s->sc_out = s->sc * s->split;
    s->gd_out = (s->drv == 1 ? s->gd_t : 1.0f) / s->split;
  }
  s->sco = s->sc_out;
  /* the 303's accent sweep capacitor: it charges from the accent envelope above ACCENT's threshold, charge 85 > 50 ms
     and discharge 150 > 100 ms by the RES pot, and moves the cutoff by a direct path (1 - RES) and the capacitor (RES) */
  float r = s->respot;
  s->wmeg *= g_wdecay;
  if (s->wmeg < 1e-15f) s->wmeg = 0.0f;
  float vin = s->wmeg * s->wamt;
  if (vin > s->wcap) s->wcap += (vin - s->wcap) * (1.0f - expf(-g_tc_ms / (85.0f - 35.0f * r)));
  s->wcap -= s->wcap * (1.0f - expf(-g_tc_ms / (150.0f - 50.0f * r)));
  if (s->wcap < 1e-15f) s->wcap = 0.0f;
  s->l0t = s->l0 + 1.5f * vin * (1.0f - r) + 2.7f * s->wcap * r;
  s->est = s->es;
}

static void bp_voice_init(void *st) {
  bp_state *s = st;
  s->dirty = 0; s->have = 0;
  s->k[K_CUT] = -1.0f;
  bp_params(s, k_defaults);
  s->split = 1.0f;
  bp_control(s);
  s->l0g = s->l0t; s->esg = s->est;
  s->ctl = CTL;
  s->note = 48.0f;
  s->inc = s->inc_t = 2.0 * 130.8128 / (4.0 * g_fs);
  s->c_meg = s->m_amp = s->m_off = g_off_n;
}

static void bp_note_on(void *st, float note, float vel) {
  bp_state *s = st;
  note = clampf(note, 0.0f, 127.0f);
  float acc = vel > ACCENT_VEL ? KLERP(BP_ACC_TAPER_Q8, s->k[K_ACC], 1.0f / (256.0f * 127.0f)) : 0.0f;   /* accent gain */
  int32_t slide = s->gate && s->active && !s->quiet;      /* (a held note that has died away starts anew) */
  if (acc > 0.0f) {
    s->c_meg = g_macc;                                     /* an accented note ignores DEC, as the 303 */
    s->m_off = g_off_a;
    s->agn = acc; s->agf = 1;
    s->k8 = K8_MULT * (0.45f + 4.0f * acc);
  } else {
    s->c_meg = decay_c(0.05f * expf(3.68887945f * s->k[K_DEC] * (1.0f / 127.0f)));   /* 50 ms x 40^DEC */
    s->m_off = g_off_n;
    s->agn = 0.0f; s->agf = 0;
    s->k8 = K8_MULT * 0.45f;
  }
  float a4 = 440.0f + (s->k[K_TUNE] - 64.0f) * (80.0f / 127.0f);
  s->inc_t = 2.0 * (double)a4 * pow(2.0, ((double)note - 69.0) / 12.0) / (4.0 * (double)g_fs);
  if (s->inc_t > 0.45) s->inc_t = 0.45;
  s->note = note;
  if (!slide) {                                            /* a slide into an open gate keeps the envelopes */
    float pos = acc > 0.0f ? KLERP(BP_ACC_POS_Q16, s->k[K_ACC], 1.0f / 65536.0f) : 0.0f;
    s->wamt = pos > 0.6f ? (pos - 0.6f) * 2.5f : 0.0f;
    if (acc > 0.0f) s->wmeg = 1.0f;
    s->m_amp = g_amp_on;
    s->inc = s->inc_t;
    s->meg = 1.0f / s->c_meg;
    s->amp = 1.0f / g_amp_on;
  }
  s->gate = 1;
  s->active = 1;
  s->quiet = 0;
}

static void bp_note_off(void *st) {
  bp_state *s = st;
  if (!s->gate) return;
  s->gate = 0;
  s->k8 = 0.0f;
  s->m_amp = s->m_off;
}

/* the audio path's memories (not the oscillator's phase) */
static void bp_clear(bp_state *s) {
  s->d1 = s->d2 = 0.0f;
  s->hpx = s->hpy = s->fbx = s->fby = s->y1 = s->y2 = s->y3 = s->y4 = 0.0f;
  for (int32_t i = 0; i < 2 * HB4_N; ++i) s->hb4[i] = 0.0f;
  for (int32_t i = 0; i < 2 * HB2_N; ++i) s->hb2[i] = 0.0f;
  s->apx = s->apy = s->h2x = s->h2y = s->tops = 0.0f;
  s->n1 = s->n2 = 0.0;
}

static void bp_sleep(bp_state *s) {
  s->active = 0; s->quiet = 0;
  s->meg = s->megs = s->rc2 = s->amp = 0.0f;
  bp_clear(s);
  s->l0g = s->l0t; s->esg = s->est;
}

static void bp_render(void *st, float *out, int32_t n) {
  bp_state *s = st;
  for (int32_t i = 0; i < n; ++i) {
    if (--s->ctl <= 0) { s->ctl = CTL; bp_control(s); }
    if (!s->active) {                                      /* the oscillator runs free, as the 303's */
      s->ph += 4.0 * s->inc;
      if (s->ph >= 1.0) s->ph -= 2.0;
      out[i] = 0.0f;
      continue;
    }
    /* the cutoff / env glide, the pitch slew, the envelopes */
    s->l0g += (s->l0t - s->l0g) * g_gld;
    s->esg += (s->est - s->esg) * g_gld;
    s->inc += (double)s->cs * (s->inc_t - s->inc);
    float meg = s->meg * s->c_meg;
    if (meg < 1e-18f) meg = 0.0f;
    s->meg = meg;
    float megs = meg + g_rc1 * (s->megs - meg);
    if (megs < 1e-18f) megs = 0.0f;
    s->megs = megs;
    float in2 = s->agf ? meg : 0.0f;
    float rc2 = in2 + g_c2 * (s->rc2 - in2);
    if (rc2 < 1e-18f) rc2 = 0.0f;
    s->rc2 = rc2;
    /* log cutoff, 70 Hz .. 20 kHz, then the ladder's coefficients from the tables */
    float l = clampf(s->l0g + s->esg * megs + s->agn * rc2, 0.0f, g_lmax) * (float)LT_SPO;
    int32_t li = (int32_t)l;
    float lf = l - (float)li;
    float b0 = g_b0[li] + (g_b0[li + 1] - g_b0[li]) * lf, b2 = b0 + b0;
    float kp = g_kp[li] + (g_kp[li + 1] - g_kp[li]) * lf;
    float k = kp * s->rs, g = s->ag + s->bg * kp;
    /* the VCA: the amp envelope plus the main envelope's share (more on an accent), through the de-clicker */
    float amp = s->amp * s->m_amp;
    if (amp < 1e-18f) amp = 0.0f;
    s->amp = amp;
    float u = amp + s->k8 * meg;
    float v3 = u - s->d2;
    float v1 = g_da1 * s->d1 + g_da2 * v3, v2 = s->d2 + g_da2 * s->d1 + g_da3 * v3;
    s->d1 = v1 + v1 - s->d1; s->d2 = v2 + v2 - s->d2;
    if (u == 0.0f && v2 < 1e-18f && v2 > -1e-18f) s->d1 = s->d2 = v2 = 0.0f;   /* (no subnormals) */
    float ads = v2 * s->sc_out;

    float inc = (float)s->inc, iinc = 1.0f / inc;
    float hpx = s->hpx, hpy = s->hpy, fbx = s->fbx, fby = s->fby, y1 = s->y1, y2 = s->y2, y3 = s->y3, y4 = s->y4;
    double ph = s->ph;
    for (int32_t j = 0; j < 4; ++j) {
      ph += s->inc;
      if (ph >= 1.0) ph -= 2.0;
      float p = (float)ph;
      float o = s->w1 * blep(p, inc, iinc);
      if (s->sq) {                                         /* the square: the saw half a cycle on, tanh-shaped */
        float sh = blep(p >= 0.0f ? p - 1.0f : p + 1.0f, inc, iinc);
        o += s->w2 * th_lookup(69.9842f * sh + 4.37f) + s->w3;
      }
      hpy = g_hp1b * (o - hpx) + g_hp1a * hpy; hpx = o;    /* the input coupling */
      float fb = k * y4;                                   /* the resonance loop's high-pass */
      fby = g_fbb * (fb - fbx) + g_fba * fby; fbx = fb;
      float y0 = hpy + hpy - fby;
      y1 += b2 * (y0 - y1 + y2);                           /* Open303's TeeBee ladder, TB_303 mode */
      y2 += b0 * (y1 - y2 - y2 + y3);
      y3 += b0 * (y2 - y3 - y3 + y4);
      y4 += b0 * (y3 - y4 - y4);
      float *h = s->hb4 + s->hb4p;
      h[0] = h[HB4_N] = 0.25f * g * y4;
      if (++s->hb4p == HB4_N) s->hb4p = 0;
      if (j & 1) {
        const float *w = s->hb4 + s->hb4p;
        float a = 0.5f * w[7] + k_hb4[0] * (w[0] + w[14]) + k_hb4[1] * (w[2] + w[12]) + k_hb4[2] * (w[4] + w[10]) +
                  k_hb4[3] * (w[6] + w[8]);
        float *h2 = s->hb2 + s->hb2p;
        h2[0] = h2[HB2_N] = a;
        if (++s->hb2p == HB2_N) s->hb2p = 0;
      }
    }
    if (!(y4 > -64.0f && y4 < 64.0f)) hpx = hpy = fbx = fby = y1 = y2 = y3 = y4 = 0.0f;   /* never NaN, never runaway */
    if (fby < 1e-20f && fby > -1e-20f) fby = 0.0f;         /* RES 0: the loop's memory dies away (no subnormals) */
    s->hpx = hpx; s->hpy = hpy; s->fbx = fbx; s->fby = fby; s->y1 = y1; s->y2 = y2; s->y3 = y3; s->y4 = y4;
    s->ph = ph;
    const float *w = s->hb2 + s->hb2p;
    float x = 0.5f * w[21];
    for (int32_t t = 0; t < 11; ++t) x += k_hb2[t] * (w[2 * t] + w[42 - 2 * t]);
    /* allpass 14 Hz, the output coupling, Open303's notch */
    float ap = g_apc * x + s->apx - g_apc * s->apy; s->apx = x; s->apy = ap;
    float h2 = g_hp2b * (ap - s->h2x) + g_hp2a * s->h2y; s->h2x = ap; s->h2y = h2;
    double nv3 = (double)h2 - s->n2;
    double nv1 = g_na1 * s->n1 + g_na2 * nv3, nv2 = s->n2 + g_na2 * s->n1 + g_na3 * nv3;
    s->n1 = nv1 + nv1 - s->n1; s->n2 = nv2 + nv2 - s->n2;
    float y = (float)((double)h2 - g_nk * nv1);
    y *= 2.0f * ads;                                       /* (the ladder's word above is g y4 / 4) */
    float tv = (y - s->tops) * g_top;                      /* the 16 kHz top rounding */
    y = tv + s->tops; s->tops = y + tv;
    if (y < 1e-18f && y > -1e-18f) y = s->tops = 0.0f;
    if (s->gd_out > 0.0f) y = th_lookup(s->gd_out * y);    /* the soft drive */
    if (!(y > -1e6f && y < 1e6f)) { bp_clear(s); y = 0.0f; }   /* never NaN: the memories start again */
    y = clampf(y, -1.0f, 1.0f);                            /* the clean output's ceiling */
    /* the VCA has closed (or a held note has died away): exact silence. As in a 303 the oscillator and the filter run on behind it, so the next
       note of a line meets a filter in motion; after RUN_ON_S without a note the voice sleeps (no CPU at rest) */
    if (s->quiet || (amp < 1e-6f && v2 < 1e-6f && v2 > -1e-6f && (!s->gate || s->k8 * meg < 1e-6f))) {
      y = 0.0f;
      s->amp = s->d1 = s->d2 = s->tops = 0.0f;
      if (++s->quiet > (int32_t)(RUN_ON_S * g_fs)) bp_sleep(s);
    }
    out[i] = y;
  }
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 2u, sizeof(bp_state), BP303_PERMILLE,   /* BP303_PERMILLE: the Makefile's, from tests/bp303_check */
  bp_init, bp_voice_init, bp_note_on, bp_note_off, bp_params, bp_render,
  {0, 0, 0, 0, 0, 0, 0, 0}};
