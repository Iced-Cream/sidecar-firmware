// plugins/ufx_galactic/galactic.c - GALACTIC: Airwindows' Galactic reverb (Chris Johnson, MIT: LICENSE.airwindows next
// to this file; github.com/airwindows/airwindows, plugins/LinuxVST/src/Galactic) as a Plugin API 3 user effect
// (sdk/rmp/rmp_api3.h) on a send slot (UFX1..UFX4): mono in, stereo wet out, 48 kHz.
//   P1 REPL  Replace: how fast new sound replaces the old; low = the tail regenerates for longer
//   P2 BRT   Brightness: the low-pass before and after the tank
//   P3 DTUN  Detune: the speed of the pre-delay's vibrato (the drifting, chorused character)
//   P4 BIG   Bigness: the size of the space (every delay time)
// Kept from Galactic: the whole signal path at its 1x rate (the vibrato pre-delay, the low-pass, three blocks of four
// delays a side each mixed by a Householder matrix, the left tank fed back from the right and the right from the
// left, the low-pass again), the delay times, the knob laws, the random vibrato speed. Changed for the Sidecar: C
// instead of the VST class; the delay memory in the slot's arena as 32-bit floats (Galactic: doubles); the send's
// mono sum feeds both sides (the stereo comes from the vibrato's two phases); always fully wet, without Galactic's
// Dry/Wet; x4 (12 dB) on the output so it sits level with the other send FX; no output dither; fixed seeds; sin()
// by a series (no libm).
// The port: MIT, Copyright 2026 trigcondition and contributors. Galactic: Copyright (c) 2018 Chris Johnson, MIT.
#include "rmp/rmp_api3.h"

/* Galactic's buffer sizes, a side: I J K L, A B C D, E F G H, and the pre-delay M */
enum { nI = 6480, nJ = 3660, nK = 1720, nL = 680, nA = 9700, nB = 6000, nC = 2320, nD = 940,
       nE = 15220, nF = 8460, nG = 4540, nH = 3200, nM = 3111 };
#define SIDE_FLOATS (nI + nJ + nK + nL + nA + nB + nC + nD + nE + nF + nG + nH + nM)
#define OUT_GAIN 4.0

typedef struct {
  int ready;
  float *a[2][13];                   /* [side][I J K L A B C D E F G H M] */
  int count[13], delay[13];
  double iirA[2], iirB[2], fb[2][4];
  double vibM, oldfpd;
  uint32_t fpd[2];
  float p[4];                        /* P1..P4 as params() gave them (a host without b->p) */
} gx;
enum { I, J, K, L, A, B, C, D, E, F, G, H, M };

static inline double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline double sin_(double a) {   /* sin(a), a >= 0 in radians (1e-9) */
  double x = a * 0.15915494309189535;
  x -= (double)(long long)x;
  if (x > 0.5) x -= 1.0;
  if (x > 0.25) x = 0.5 - x;
  else if (x < -0.25) x = -0.5 - x;
  double r = x * 6.283185307179586, r2 = r * r;
  return r * (1.0 + r2 * (-1.0 / 6.0 + r2 * (1.0 / 120.0 + r2 * (-1.0 / 5040.0 + r2 * (1.0 / 362880.0 + r2 * (-1.0 / 39916800.0 + r2 * (1.0 / 6227020800.0)))))));
}

static int32_t gx_init(const rmp_host *h) { return h->sample_rate == 48000.0f ? 0 : 1; }

static void gx_voice_init(void *st) {
  gx *s = st;
  uint8_t *z = st;
  for (uint32_t i = 0; i < sizeof(gx); ++i) z[i] = 0;
  s->p[0] = 38.0f, s->p[1] = 64.0f, s->p[2] = 64.0f, s->p[3] = 127.0f;
  for (int i = 0; i < 13; ++i) s->count[i] = 1;
  s->vibM = 3.0, s->oldfpd = 429496.7295;
  s->fpd[0] = 0x9E3779B9u, s->fpd[1] = 0x85EBCA6Bu;   /* (Galactic: rand() at the start) */
}

static void gx_params(void *st, const float *p) {
  gx *s = st;
  for (int k = 0; k < 4; ++k) s->p[k] = p[k];
}

static void gx_attach(void *st, const rmp3_voice *v) {
  static const int k_n[13] = {nI, nJ, nK, nL, nA, nB, nC, nD, nE, nF, nG, nH, nM};
  gx *s = st;
  s->ready = 0;
  if (!v->arena || v->arena_bytes < 2u * SIDE_FLOATS * sizeof(float)) return;
  float *at = (float *)v->arena;   /* (zeroed by the host when the slot loads) */
  for (int c = 0; c < 2; ++c)
    for (int i = 0; i < 13; ++i) s->a[c][i] = at, at += k_n[i];
  s->ready = 1;
}

/* one of Galactic's delays: write, step, read (its own index arithmetic) */
#define STEP(i) do { s->count[i]++; if (s->count[i] < 0 || s->count[i] > s->delay[i]) s->count[i] = 0; } while (0)
#define OUT(c, i) ((double)s->a[c][i][s->count[i] - ((s->count[i] > s->delay[i]) ? s->delay[i] + 1 : 0)])

static void gx_render(void *st, const rmp3_block *b) {
  static const double k_time[12] = {3407.0, 1823.0, 859.0, 331.0, 4801.0, 2909.0, 1153.0, 461.0, 7607.0, 4217.0, 2269.0, 1597.0};
  gx *s = st;
  float *l = b->out[0], *r = b->out[1];
  if (!s->ready) {
    for (int32_t i = 0; i < b->n; ++i) l[i] = 0.0f;
    if (r) for (int32_t i = 0; i < b->n; ++i) r[i] = 0.0f;
    return;
  }
  const float *p = b->p ? b->p : s->p;
  double pa = clampd(p[0], 0.0, 127.0) / 127.0, pb = clampd(p[1], 0.0, 127.0) / 127.0,
         pc = clampd(p[2], 0.0, 127.0) / 127.0, pd = clampd(p[3], 0.0, 127.0) / 127.0;
  double regen = 0.0625 + ((1.0 - pa) * 0.0625);
  double attenuate = (1.0 - (regen / 0.125)) * 1.333;
  double lowpass = (1.00001 - (1.0 - pb)) * (1.00001 - (1.0 - pb)) / 1.0432810619;   /* / sqrt(48000 / 44100) */
  double drift = pc * pc * pc * 0.001;
  double size = (pd * 1.77) + 0.1;
  for (int i = 0; i < 12; ++i) s->delay[i] = (int)(k_time[i] * size);
  s->delay[M] = 256;

  for (int32_t n = 0; n < b->n; ++n) {
    double x = b->in[0] ? b->in[0][n] : 0.0;
    if (b->in[1]) x = 0.5 * (x + b->in[1][n]);
    if (!(x == x)) x = 0.0;   /* (a NaN would stay in the tank for good) */
    double in[2] = {x, x};
    for (int c = 0; c < 2; ++c)
      if (in[c] > -1.18e-23 && in[c] < 1.18e-23) in[c] = s->fpd[c] * 1.18e-17;
    s->vibM += (s->oldfpd * drift);
    if (s->vibM > (3.141592653589793238 * 2.0)) {
      s->vibM = 0.0;
      s->oldfpd = 0.4294967295 + (s->fpd[0] * 0.0000000000618);
    }
    /* the pre-delay that applies the vibrato */
    s->a[0][M][s->count[M]] = (float)(in[0] * attenuate);
    s->a[1][M][s->count[M]] = (float)(in[1] * attenuate);
    STEP(M);
    for (int c = 0; c < 2; ++c) {
      double off = (sin_(s->vibM + (c ? 3.141592653589793238 / 2.0 : 0.0)) + 1.0) * 127;
      int wk = s->count[M] + (int)off;
      double fr = off - (double)(int)off;
      const float *m = s->a[c][M];
      double y = m[wk - ((wk > s->delay[M]) ? s->delay[M] + 1 : 0)] * (1 - fr);
      y += m[wk + 1 - ((wk + 1 > s->delay[M]) ? s->delay[M] + 1 : 0)] * fr;
      s->iirA[c] = (s->iirA[c] * (1.0 - lowpass)) + (y * lowpass);   /* the initial filter */
      in[c] = s->iirA[c];
    }
    /* the first block: each side takes the other side's feedback */
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < 4; ++i) s->a[c][I + i][s->count[I + i]] = (float)(in[c] + (s->fb[1 - c][i] * regen));
    STEP(I); STEP(J); STEP(K); STEP(L);
    double o[2][4];
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < 4; ++i) o[c][i] = OUT(c, I + i);
    for (int c = 0; c < 2; ++c) {
      double sum = o[c][0] + o[c][1] + o[c][2] + o[c][3];
      for (int i = 0; i < 4; ++i) s->a[c][A + i][s->count[A + i]] = (float)(o[c][i] - (sum - o[c][i]));
    }
    STEP(A); STEP(B); STEP(C); STEP(D);
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < 4; ++i) o[c][i] = OUT(c, A + i);
    for (int c = 0; c < 2; ++c) {
      double sum = o[c][0] + o[c][1] + o[c][2] + o[c][3];
      for (int i = 0; i < 4; ++i) s->a[c][E + i][s->count[E + i]] = (float)(o[c][i] - (sum - o[c][i]));
    }
    STEP(E); STEP(F); STEP(G); STEP(H);
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < 4; ++i) o[c][i] = OUT(c, E + i);
    double out[2];
    for (int c = 0; c < 2; ++c) {
      double sum = o[c][0] + o[c][1] + o[c][2] + o[c][3];
      for (int i = 0; i < 4; ++i) s->fb[c][i] = o[c][i] - (sum - o[c][i]);   /* fed back into the input again, a bit */
      s->iirB[c] = (s->iirB[c] * (1.0 - lowpass)) + ((sum / 8.0) * lowpass);   /* the end filter */
      out[c] = s->iirB[c] * OUT_GAIN;
    }
    for (int c = 0; c < 2; ++c)   /* the random numbers step at the end of a frame (Galactic: in its dither) */
      s->fpd[c] ^= s->fpd[c] << 13, s->fpd[c] ^= s->fpd[c] >> 17, s->fpd[c] ^= s->fpd[c] << 5;
    if (r) l[n] = (float)out[0], r[n] = (float)out[1];
    else l[n] = (float)(0.5 * (out[0] + out[1]));   /* a mono route: the mix */
  }
}

static const rmp3_plugin g3 = {
  .magic = RMP3_MAGIC, .size = sizeof(rmp3_plugin), .minor = RMP3_MINOR,
  .kind = RMP3_KIND_EFFECT, .places = RMP3_AT_SEND,
  .needs = RMP3_CAP_ARENA | RMP3_CAP_EFFECT,
  .wants = RMP3_CAP_STEREO | RMP3_CAP_BLOCK_P,
  .voice_min = 2u * SIDE_FLOATS * sizeof(float),
  .voice_want = 2u * SIDE_FLOATS * sizeof(float),
  .in_channels = 1, .out_channels = 2,
  .voice_attach = gx_attach, .render = gx_render,
};

RMP_ENTRY const rmp_plugin rmp_entry = {
  .magic = RMP_PLUGIN_MAGIC,
  .api = 3u,                     /* API 3 only */
  .state_bytes = sizeof(gx), .cpu_permille = 20,
  .init = gx_init, .voice_init = gx_voice_init, .params = gx_params,
  .reserved = { [RMP_P_V3] = RMP_FN(&g3) },
};
