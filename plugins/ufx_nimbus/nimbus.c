// plugins/ufx_nimbus/nimbus.c - NIMBUS: a big ambient reverb with an ENSEMBLE, a pad that follows the harmony of what
// is sent to it (the idea of the Strymon Cloudburst's ensemble; no code from anywhere). A Plugin API 3 user effect
// (sdk/rmp/rmp_api3.h) on a send slot (UFX1..UFX4): mono in, stereo wet out, 48 kHz.
//   P1 DCY   the decay time, 0.5 s .. 64 s (exponential)
//   P2 ENS   the ensemble: how much of the pad goes into the reverb (0: the reverb alone)
//   P3 TONE  the damping in the reverb's loop, 500 Hz .. 18 kHz
//   P4 MOD   the depth of the delay lines' modulation, x0 .. x2
// ENSEMBLE  48 band analysers a semitone apart (131 Hz .. 1976 Hz, C3 .. B6), each a complex one-pole resonator.
//           Every 16 frames a band's level and its real frequency (the phase's turn over those 16 frames) are read.
//           Bands that are a peak among their neighbours are kept (level x level / loudest), each with a slow swell
//           (0.25 s) and a long release (a quarter of the decay time, 0.2 s .. 3 s), and each drives a sine
//           oscillator at the frequency it heard, so nothing is retuned to a grid. The sum goes through a three-voice string-ensemble chorus.
// REVERB    a 25 ms pre-delay, four modulated allpass diffusers a side, then 16 delay lines mixed by a Hadamard
//           matrix, each line: modulated read -> allpass -> damping low-pass -> the gain for the decay time.
// The arena holds every delay line (610 KB). No libm, no allocation, no host call in render; denormals flushed.
// MIT, Copyright 2026 trigcondition and contributors (LICENSE next to this file).
#include "rmp/rmp_api3.h"

#define FS 48000.0f
#define TWO_PI 6.28318531f
#define BANDS 48
#define CTL 16
#define LINES 16

typedef struct { float *b; uint32_t mask, w; } ring;   /* a power-of-two delay line in the arena */
typedef struct { float s, c, e; } lfo;                 /* a sine as a coupled pair: s += e c, c -= e s */

typedef struct {
  int ready;
  /* the ensemble */
  float cr[BANDS], ci[BANDS], zr[BANDS], zi[BANDS], pr[BANDS], pi[BANDS];   /* the resonators (pr/pi: 16 frames ago) */
  float cb[BANDS], sb[BANDS];        /* cos, sin of the centre's turn over 16 frames */
  float w[BANDS], w0[BANDS];         /* the oscillator's and the centre's frequency, turns a frame */
  float lvl[BANDS], amp[BANDS], ph[BANDS], ing[BANDS];
  float atk, rel;
  ring ch;
  lfo c1, c2;
  int ctl, ens_on;
  /* the reverb */
  ring pre[2], dif[2][4], line[LINES], lap[LINES];
  lfo dl[2][4], ll[LINES];
  float len[LINES], depth[LINES], gain[LINES], gain_t[LINES], lp[LINES], hp[LINES];
  float damp, damp_t, hpc, ens, ens_t, mod, mod_t;
  float p[4];                        /* P1..P4 as params() gave them (a host without b->p) */
  float seen[4];                     /* the knobs the targets were last made from */
} nb;

static const uint16_t k_len[LINES] = {1297, 1481, 1699, 1907, 2141, 2383, 2617, 2861, 3109, 3389, 3659, 3931, 4253, 4603, 4967, 5351};
static const uint16_t k_lap[LINES] = {211, 293, 347, 409, 229, 311, 373, 433, 251, 331, 389, 449, 269, 337, 397, 461};
static const uint16_t k_dif[2][4] = {{229, 173, 613, 443}, {241, 181, 587, 461}};
#define ARENA_FLOATS (2u * 2048u + 8u * 1024u + LINES * 8192u + LINES * 512u + 1024u)

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float flush(float x) { return x > -1e-18f && x < 1e-18f ? 0.0f : x; }
static inline float exp2_(float x) {   /* 2^x: the integer part as the exponent bits, the fraction by a series (2e-6) */
  x = clampf(x, -126.0f, 126.0f);
  int i = (int)x;
  if ((float)i > x) --i;
  float f = x - (float)i;
  union { float f; uint32_t u; } v;
  v.f = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * (0.00133336f + f * 0.00015404f)))));
  v.u += (uint32_t)i << 23;
  return v.f;
}
static inline float sin_t(float x) {   /* sin(2 pi x), x in turns (4e-6) */
  x -= (float)(int)x;
  if (x > 0.5f) x -= 1.0f;
  else if (x < -0.5f) x += 1.0f;
  if (x > 0.25f) x = 0.5f - x;
  else if (x < -0.25f) x = -0.5f - x;
  float a = x * TWO_PI, a2 = a * a;
  return a * (1.0f + a2 * (-0.16666667f + a2 * (0.0083333310f + a2 * (-0.00019840874f + a2 * 2.7525562e-6f))));
}
/* a knob's value toward its target, and onto it when near (a one-pole alone stops just short, at a denormal) */
static inline float glide(float x, float t) { float d = t - x; return d > -1e-6f && d < 1e-6f ? t : x + 0.02f * d; }
static inline float cos_t(float x) { return sin_t(x + 0.25f); }
static inline float rnd(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return (float)(*s >> 8) * (1.0f / 16777216.0f); }
static inline void lfo_set(lfo *o, float phase_t, float hz) { o->s = sin_t(phase_t), o->c = cos_t(phase_t), o->e = TWO_PI * hz / FS; }
static inline float lfo_run(lfo *o) { o->s += o->e * o->c, o->c -= o->e * o->s; return o->s; }

static inline void put(ring *r, float x) { r->b[r->w] = flush(x), r->w = (r->w + 1u) & r->mask; }   /* (no denormals into any line) */
static inline float tap(const ring *r, uint32_t d) { return r->b[(r->w - d) & r->mask]; }
static inline float read(const ring *r, float d) {   /* d frames ago (>= 2), cubic */
  uint32_t i = (uint32_t)d;
  float f = d - (float)i;
  float xm = tap(r, i - 1u), x0 = tap(r, i), x1 = tap(r, i + 1u), x2 = tap(r, i + 2u);
  float c = (x1 - xm) * 0.5f, v = x0 - x1, w = c + v, a = w + v + (x2 - x0) * 0.5f, b = w + a;
  return ((a * f - b) * f + c) * f + x0;
}

static int32_t nb_init(const rmp_host *h) { return h->sample_rate == 48000.0f ? 0 : 1; }

static void nb_voice_init(void *st) {
  nb *s = st;
  uint8_t *z = st;
  for (uint32_t i = 0; i < sizeof(nb); ++i) z[i] = 0;
  s->p[0] = 96.0f, s->p[1] = 64.0f, s->p[2] = 80.0f, s->p[3] = 64.0f;
  for (int k = 0; k < 4; ++k) s->seen[k] = -1.0f;
  for (int k = 0; k < BANDS; ++k) {
    float f = 130.81f * exp2_((float)k / 12.0f), bw = f / 24.0f;
    float r = exp2_(-3.14159265f * bw / FS * 1.44269504f);
    s->w0[k] = s->w[k] = f / FS;
    s->cr[k] = r * cos_t(s->w0[k]), s->ci[k] = r * sin_t(s->w0[k]);
    s->cb[k] = cos_t(s->w0[k] * (float)CTL), s->sb[k] = sin_t(s->w0[k] * (float)CTL);
    s->ing[k] = 2.0f * (1.0f - r);   /* a sine of level A at the centre reads A */
  }
  s->atk = 1.0f - exp2_(-(float)CTL / (0.25f * FS) * 1.44269504f);
  lfo_set(&s->c1, 0.0f, 0.63f), lfo_set(&s->c2, 0.0f, 5.9f);
  uint32_t seed = 0x051DECA2u;
  for (int c = 0; c < 2; ++c)
    for (int i = 0; i < 4; ++i) {
      float ph = rnd(&seed), hz = 0.15f + 0.5f * rnd(&seed);
      lfo_set(&s->dl[c][i], ph, hz);
    }
  for (int i = 0; i < LINES; ++i) {
    float ph = rnd(&seed), hz = 0.08f + 0.9f * rnd(&seed);
    lfo_set(&s->ll[i], ph, hz);
    s->len[i] = (float)k_len[i];
    s->depth[i] = 12.0f + 20.0f * rnd(&seed);
  }
  s->hpc = 1.0f - exp2_(-TWO_PI * 45.0f / FS * 1.44269504f);
}

static void nb_params(void *st, const float *p) {
  nb *s = st;
  for (int k = 0; k < 4; ++k) s->p[k] = p[k];
}

static float *carve(float **at, ring *r, uint32_t n) {
  r->b = *at, r->mask = n - 1u, r->w = 0;
  *at += n;
  return r->b;
}
static void nb_attach(void *st, const rmp3_voice *v) {
  nb *s = st;
  s->ready = 0;
  if (!v->arena || v->arena_bytes < ARENA_FLOATS * sizeof(float)) return;
  float *at = (float *)v->arena;   /* (zeroed by the host when the slot loads) */
  carve(&at, &s->pre[0], 2048u), carve(&at, &s->pre[1], 2048u);
  for (int c = 0; c < 2; ++c)
    for (int i = 0; i < 4; ++i) carve(&at, &s->dif[c][i], 1024u);
  for (int i = 0; i < LINES; ++i) carve(&at, &s->line[i], 8192u);
  for (int i = 0; i < LINES; ++i) carve(&at, &s->lap[i], 512u);
  carve(&at, &s->ch, 1024u);
  s->ready = 1;
}

/* the knobs' targets, only when one moved */
static void targets(nb *s, const float *p) {
  if (p[0] == s->seen[0] && p[1] == s->seen[1] && p[2] == s->seen[2] && p[3] == s->seen[3]) return;
  int first = s->seen[0] < 0.0f;
  for (int k = 0; k < 4; ++k) s->seen[k] = p[k];
  float t60 = 0.5f * exp2_(7.0f * clampf(p[0], 0.0f, 127.0f) / 127.0f);
  for (int i = 0; i < LINES; ++i)   /* 10^(-3 n / (T60 fs)) */
    s->gain_t[i] = exp2_(-9.96578f * (s->len[i] + 34.0f + (float)k_lap[i]) / (t60 * FS));
  /* the pad lets go with the reverb: a quarter of the decay time, 0.2 s .. 3 s */
  s->rel = 1.0f - exp2_(-(float)CTL / (clampf(0.25f * t60, 0.2f, 3.0f) * FS) * 1.44269504f);
  s->ens_t = clampf(p[1], 0.0f, 127.0f) / 127.0f;
  float fc = clampf(500.0f * exp2_(5.3f * clampf(p[2], 0.0f, 127.0f) / 127.0f), 500.0f, 18000.0f);
  s->damp_t = 1.0f - exp2_(-TWO_PI * fc / FS * 1.44269504f);
  s->mod_t = 2.0f * clampf(p[3], 0.0f, 127.0f) / 127.0f;
  if (first) {
    for (int i = 0; i < LINES; ++i) s->gain[i] = s->gain_t[i];
    s->ens = s->ens_t, s->damp = s->damp_t, s->mod = s->mod_t;
  }
}

/* every 16 frames: the bands' levels and frequencies */
static void control(nb *s) {
  float a[BANDS], top = 1e-9f;
  for (int k = 0; k < BANDS; ++k) {
    s->zr[k] = flush(s->zr[k]), s->zi[k] = flush(s->zi[k]);
    a[k] = __builtin_sqrtf(s->zr[k] * s->zr[k] + s->zi[k] * s->zi[k]);
    if (a[k] > top) top = a[k];
  }
  for (int k = 0; k < BANDS; ++k) {
    /* a band drives its oscillator only where it is a peak among its neighbours (a tone also rings the bands beside
       its own, and a noise burst rings them all: neither may turn into a cluster of semitones) */
    float nbr = k > 0 ? a[k - 1] : 0.0f;
    if (k < BANDS - 1 && a[k + 1] > nbr) nbr = a[k + 1];
    float pk = nbr > 0.0f ? clampf((a[k] / nbr - 1.0f) * 4.0f, 0.0f, 1.0f) : 1.0f;
    float target = a[k] * (a[k] / top) * pk;
    s->lvl[k] += (target > s->lvl[k] ? s->atk : s->rel) * (target - s->lvl[k]);
    if (s->lvl[k] < 1e-7f) {
      s->lvl[k] = 0.0f;
      if (s->amp[k] < 1e-7f) s->amp[k] = 0.0f;   /* (its own one-pole would stop at a denormal) */
    }
    /* the band's real frequency, only while it is driven (a ringing-out resonator drifts to its own centre): the
       turn since 16 frames ago, less the centre's own turn, is small: its angle by the arctangent's series */
    if (a[k] > 1e-4f && target >= 0.5f * s->lvl[k]) {
      float re = s->zr[k] * s->pr[k] + s->zi[k] * s->pi[k], im = s->zi[k] * s->pr[k] - s->zr[k] * s->pi[k];
      float re2 = re * s->cb[k] + im * s->sb[k], im2 = im * s->cb[k] - re * s->sb[k];
      if (re2 > 0.0f) {
        float t = clampf(im2 / re2, -0.5f, 0.5f);
        float d = (t - t * t * t * (1.0f / 3.0f)) / (TWO_PI * (float)CTL);   /* turns a frame off the centre */
        float lim = 0.04f * s->w0[k];                                        /* at most ~2/3 semitone */
        s->w[k] += 0.1f * (s->w0[k] + clampf(d, -lim, lim) - s->w[k]);
      }
    }
    s->pr[k] = s->zr[k], s->pi[k] = s->zi[k];
  }
  for (int i = 0; i < LINES; ++i) {
    s->gain[i] += 0.02f * (s->gain_t[i] - s->gain[i]);
    s->lp[i] = flush(s->lp[i]), s->hp[i] = flush(s->hp[i]);
  }
  s->ens = glide(s->ens, s->ens_t), s->damp = glide(s->damp, s->damp_t), s->mod = glide(s->mod, s->mod_t);
  /* the coupled sines keep their size only nearly: pulled back to 1 */
  float n1 = 1.5f - 0.5f * (s->c1.s * s->c1.s + s->c1.c * s->c1.c), n2 = 1.5f - 0.5f * (s->c2.s * s->c2.s + s->c2.c * s->c2.c);
  s->c1.s *= n1, s->c1.c *= n1, s->c2.s *= n2, s->c2.c *= n2;
}

static void nb_render(void *st, const rmp3_block *b) {
  nb *s = st;
  float *l = b->out[0], *r = b->out[1];
  if (!s->ready) {
    for (int32_t i = 0; i < b->n; ++i) l[i] = 0.0f;
    if (r) for (int32_t i = 0; i < b->n; ++i) r[i] = 0.0f;
    return;
  }
  targets(s, b->p ? b->p : s->p);
  static const float k_c3[3] = {1.0f, -0.5f, -0.5f}, k_s3[3] = {0.0f, 0.8660254f, -0.8660254f};   /* 0, 120, 240 degrees */
  for (int32_t i = 0; i < b->n; ++i) {
    float x = b->in[0] ? b->in[0][i] : 0.0f;
    if (b->in[1]) x = 0.5f * (x + b->in[1][i]);
    if (!(x == x)) x = 0.0f;   /* (a NaN would stay in the lines for good) */
    /* ---- the ensemble */
    float pl = 0.0f, pr = 0.0f;
    int ens_on = s->ens > 0.0005f || s->ens_t > 0.0f;
    if (!ens_on && s->ens_on) {   /* ENS reached 0: forget the pad, so that ENS up again starts from what plays then */
      for (int k = 0; k < BANDS; ++k) s->zr[k] = s->zi[k] = s->pr[k] = s->pi[k] = s->lvl[k] = s->amp[k] = 0.0f;
      for (uint32_t k = 0; k <= s->ch.mask; ++k) s->ch.b[k] = 0.0f;
    }
    s->ens_on = ens_on;
    if (ens_on) {
      for (int k = 0; k < BANDS; ++k) {
        float zr = s->zr[k] * s->cr[k] - s->zi[k] * s->ci[k] + x * s->ing[k], zi = s->zr[k] * s->ci[k] + s->zi[k] * s->cr[k];
        s->zr[k] = zr, s->zi[k] = zi;
      }
      float pad = 0.0f;
      for (int k = 0; k < BANDS; ++k) {
        s->amp[k] += 0.002f * (s->lvl[k] - s->amp[k]);   /* no steps at the control rate */
        float ph = s->ph[k] + s->w[k];
        ph = ph > 0.5f ? ph - 1.0f : ph;
        s->ph[k] = ph;
        float y = ph * (8.0f - 16.0f * __builtin_fabsf(ph));          /* a sine by two parabolas, no branch (0.1 %) */
        pad += s->amp[k] * (y + 0.225f * (y * __builtin_fabsf(y) - y));
      }
      /* the string-ensemble chorus: three taps, a slow and a fast sine each, 120 degrees apart */
      put(&s->ch, pad);
      lfo_run(&s->c1), lfo_run(&s->c2);
      float t[3];
      for (int j = 0; j < 3; ++j)
        t[j] = read(&s->ch, 600.0f + 190.0f * (s->c1.s * k_c3[j] + s->c1.c * k_s3[j]) + 22.0f * (s->c2.s * k_c3[j] + s->c2.c * k_s3[j]));
      pl = 0.6f * (t[0] + 0.5f * t[1]), pr = 0.6f * (t[2] + 0.5f * t[1]);
    }
    if (++s->ctl >= CTL) s->ctl = 0, control(s);
    /* ---- the reverb */
    float dry = x * (1.0f - 0.5f * s->ens);
    put(&s->pre[0], dry + s->ens * pl), put(&s->pre[1], dry + s->ens * pr);
    float in[2] = {tap(&s->pre[0], 1200u), tap(&s->pre[1], 1200u)};
    for (int c = 0; c < 2; ++c)
      for (int j = 0; j < 4; ++j) {
        ring *d = &s->dif[c][j];
        float g = j < 2 ? 0.72f : 0.62f;
        float y = read(d, (float)k_dif[c][j] + 8.0f + 6.0f * lfo_run(&s->dl[c][j]));
        float v = in[c] + g * y;
        put(d, v);
        in[c] = y - g * v;
      }
    float v[LINES];
    for (int j = 0; j < LINES; ++j) {
      float y = read(&s->line[j], s->len[j] + 34.0f + s->mod * s->depth[j] * lfo_run(&s->ll[j]));
      ring *a = &s->lap[j];
      float ya = tap(a, k_lap[j]), va = y + 0.5f * ya;
      put(a, va);
      y = ya - 0.5f * va;
      s->lp[j] += s->damp * (y - s->lp[j]);      /* the damping */
      s->hp[j] += s->hpc * (s->lp[j] - s->hp[j]);   /* no rumble building up in the loop */
      v[j] = (s->lp[j] - s->hp[j]) * s->gain[j];
    }
    float ol = 0.0f, orr = 0.0f;
    for (int j = 0; j < LINES; ++j) {
      ol += (j & 1) ? -v[j] : v[j];
      orr += (j & 2) ? -v[j] : v[j];
    }
    /* Hadamard (fast Walsh), normalised: every line feeds every other */
    for (int h = 1; h < LINES; h <<= 1)
      for (int j = 0; j < LINES; j += h << 1)
        for (int k = j; k < j + h; ++k) {
          float p = v[k], q = v[k + h];
          v[k] = p + q, v[k + h] = p - q;
        }
    for (int j = 0; j < LINES; ++j) put(&s->line[j], v[j] * 0.25f + in[j & 1] * 0.25f);
    if (r) l[i] = ol, r[i] = orr;
    else l[i] = 0.5f * (ol + orr);   /* a mono route: the mix */
  }
}

static const rmp3_plugin g3 = {
  .magic = RMP3_MAGIC, .size = sizeof(rmp3_plugin), .minor = RMP3_MINOR,
  .kind = RMP3_KIND_EFFECT, .places = RMP3_AT_SEND,
  .needs = RMP3_CAP_ARENA | RMP3_CAP_EFFECT,
  .wants = RMP3_CAP_STEREO | RMP3_CAP_BLOCK_P,
  .voice_min = ARENA_FLOATS * sizeof(float),
  .voice_want = ARENA_FLOATS * sizeof(float),
  .in_channels = 1, .out_channels = 2,
  .voice_attach = nb_attach, .render = nb_render,
};

RMP_ENTRY const rmp_plugin rmp_entry = {
  .magic = RMP_PLUGIN_MAGIC,
  .api = 3u,                     /* API 3 only */
  .state_bytes = sizeof(nb), .cpu_permille = 60,
  .init = nb_init, .voice_init = nb_voice_init, .params = nb_params,
  .reserved = { [RMP_P_V3] = RMP_FN(&g3) },
};
