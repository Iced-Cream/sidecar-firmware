// plugins/sophie/sophie.c - SOPHIE: metallic FM percussion (kicks, clangs, zaps, bells, shards), one Rytm track a
// state; a hit rings on under the next one (two hits a state), a poly track plays its notes on instances (manifest
// max_voices 8).
// SPDX-License-Identifier: MIT
// The DSP is Sophie's (Matt Estela, MIT: LICENSE.sophie; NOTICE has the source and commit, v0.5.1): a carrier and
// three modulators at non-integer ratios that Color moves, operator feedback, a fast bipolar pitch sweep, an
// audible-duration amplitude envelope, four operator structures (Fuse, Stack, Split, Shard) and the kick topology
// (a tuned sine body with a quiet folded / FM texture and a click), then the digital stage in Sophie's order:
// sample-and-hold decimation with bit-depth reduction, a bounded soft-clip drive, the Ring short feedback delay
// (a comb at a few ms) and last a state-variable filter (LPF, HPF, notch, DJ sweep, BPF). Each hit takes a
// snapshot of the knobs, as Sophie's voices do: a knob moved while a hit rings shapes the next hit.
// Adapted for the Sidecar (Copyright 2026 trigcondition and contributors, MIT): one track instead of a 16-pad kit
// (the kick topology, Sophie's pad 1, is the KICK models of the MODL list; a pad's note offset is TUN), two hits
// a state instead of a 12-voice pool (a third hit fades the oldest out over 2 ms), 48 kHz with the one-pole
// coefficients converted from Sophie's 44.1 kHz and the decimation held for the same time, the per-hit constants
// worked out at the trigger instead of per sample, sin and 2^x in the sample loop as polynomials accurate to float
// (fsin, fexp2: newlib's were 60 % of the time), a phase wrap that holds at any ratio, LEVEL left at Sophie's
// 100 % (use the box's own level), no allocation, everything render() writes in the state.
//   P1 TUN (64 = 0, two steps a semitone)  P2 MODL  P3 COLR  P4 METL  P5 FDBK  P6 SWP (64 = 0)  P7 DEC  P8 CRSH
//   P9 DRV  P10 CUT  P11 RES  P12 FLT  P13 RTIM ring time  P14 RFB ring feedback  P15 RMIX ring mix  P16 RTON ring tone
#include <math.h>
#include "rmp/rmp_api.h"
#include "sophie.h"

#define SOPH_PI 3.14159265358979323846f
#define SOPH_TAU (2.0f * SOPH_PI)
#define SOPH_OUT_GAIN 0.5f   /* one hit at Sophie's level 100 % lands near the other machines' level */

static const rmp_host *g_host;
static float g_fs = 48000.0f;
static float g_rate = 44100.0f / 48000.0f;   /* Sophie's 44.1 kHz per-sample coefficients, converted */

static inline float clampf(float x, float lo, float hi) { return x > lo ? (x < hi ? x : hi) : lo; }   /* NaN -> lo */
static inline float n01(float p) { return clampf(p * (1.0f / 127.0f), 0.0f, 1.0f); }
static inline int32_t zone(float p, int32_t n) {   /* a list knob's entry, as the box reads it: (value * n) >> 7 */
  int32_t z = (int32_t)(clampf(p, 0.0f, 127.0f) * (float)n * (1.0f / 128.0f));
  return z < 0 ? 0 : (z >= n ? n - 1 : z);
}
static inline float wrap_phase(float x) {
  if (x >= SOPH_TAU) x -= SOPH_TAU;
  if (x < 0.0f) x += SOPH_TAU;
  if (x >= SOPH_TAU || x < 0.0f) x -= SOPH_TAU * __builtin_floorf(x * (1.0f / SOPH_TAU));   /* a ratio of more than one turn */
  return x;
}
static inline float soft_clip(float x) { return x / (1.0f + __builtin_fabsf(x)); }
/* sinf and exp2f in the sample loop (plugin.mk's -ffreestanding makes libm's calls of them; newlib's were most of
   the time): sin after a two-part reduction by pi, an odd degree-9 fit on +-pi/2 (|error| < 1e-7, float's own
   rounding); 2^x from the exponent bits and a degree-6 fit of 2^f on 0..1 (relative error < 1e-7) */
static inline float fsin(float x) {
  float k = __builtin_rintf(x * 0.318309886f);
  float r = x - k * 3.14159274f;
  r -= k * -8.74227766e-8f;
  float r2 = r * r;
  float y = r + r * r2 * (-0.166666567f + r2 * (0.00833301033f + r2 * (-0.00019806202f + r2 * 2.59927819e-06f)));
  return ((int32_t)k & 1) ? -y : y;
}
static inline float fexp2(float x) {
  x = x > -126.0f ? (x < 126.0f ? x : 126.0f) : -126.0f;
  float i = __builtin_floorf(x), f = x - i;
  float p = 1.0f + f * (0.693147001f + f * (0.240229923f + f * (0.0554824131f + f * (0.00968126923f
          + f * (0.00124143112f + f * 0.000217959979f)))));
  union { uint32_t u; float f; } e = {(uint32_t)((int32_t)i + 127) << 23};
  return p * e.f;
}
static inline uint32_t xorshift32(uint32_t *state) {
  uint32_t x = *state ? *state : 0x6d2b79f5u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return *state = x;
}
static inline float noise_bipolar(uint32_t *state) {
  return (float)(xorshift32(state) & 0x00ffffffu) * (2.0f / 16777215.0f) - 1.0f;
}
static inline float one_pole(float c44) { return 1.0f - powf(1.0f - c44, g_rate); }   /* Sophie's per-sample c */

/* the factory kick (Sophie's pad 1): TUN -5 st, KICK, COLR 24, METL 8, FDBK 6, SWP 55, DEC 0.28 s, DRV 58 */
static const float k_defaults[RMP_PARAMS] = {54, 72, 30, 10, 8, 99, 58, 0, 74, 127, 0, 12, 77, 0, 0, 89};

static int32_t soph_init(const rmp_host *h) {
  g_host = h;
  g_fs = h->sample_rate > 1000.0f ? h->sample_rate : 48000.0f;
  g_rate = 44100.0f / g_fs;
  return 0;
}

static void soph_params(void *st, const float *p) {
  sophie_patch *q = &((sophie_state *)st)->patch;
  q->tune = clampf((clampf(p[0], 0.0f, 127.0f) - 64.0f) * 0.5f, -32.0f, 31.5f);   /* semitones, two steps each */
  int32_t m = zone(p[1], 8);                                   /* FUSE STACK SPLIT SHARD, then the same as KICK */
  q->model = m & 3;
  q->kick = m >= 4;
  q->color = n01(p[2]) * 100.0f;
  q->metal = n01(p[3]) * 100.0f;
  q->feedback = n01(p[4]) * 100.0f;
  q->sweep = clampf((clampf(p[5], 0.0f, 127.0f) - 64.0f) * (100.0f / 63.0f), -100.0f, 100.0f);
  q->decay = 0.03f * expf(4.8928523f * n01(p[6]));            /* 0.03 .. 4 s, exponential */
  q->crush = n01(p[7]) * 100.0f;
  q->drive = n01(p[8]) * 100.0f;
  q->cutoff = n01(p[9]) * 100.0f;
  q->resonance = n01(p[10]) * 100.0f;
  q->filter_type = zone(p[11], 5);                             /* LPF HPF NOTCH DJ BPF (Sophie's own numbers) */
  q->ring_time = 0.5f * expf(4.0943446f * n01(p[12]));         /* 0.5 .. 30 ms, exponential */
  q->ring_feedback = n01(p[13]) * 95.0f;
  q->ring_mix = n01(p[14]) * 100.0f;
  q->ring_tone = n01(p[15]) * 100.0f;
}

static void soph_voice_init(void *st) {
  sophie_state *s = st;
  s->rng = 0x534f5048u;
  soph_params(s, k_defaults);
}

static float midi_hz(float note) { return 440.0f * exp2f((note - 69.0f) / 12.0f); }

/* Sophie's trigger_voice, the patch turned into the hit's constants (the values render_voice works out per sample) */
static void trigger(sophie_state *s, sophie_hit *v, float note, float vel) {
  const sophie_patch *p = &s->patch;
  uint32_t seed = xorshift32(&s->rng) ^ ((uint32_t)note << 16) ^ (uint32_t)(vel * 127.0f);
  for (uint32_t *w = (uint32_t *)v, *e = (uint32_t *)(v + 1); w < e; ++w) *w = 0;   /* memset, delay buffer too */
  v->active = 1;
  v->rng = seed;
  v->velocity = clampf(vel, 0.01f, 1.0f);
  /* note 60 plays as Sophie's note 36, pad 1's; TUN shifts it as Sophie's Tune does */
  v->base_hz = clampf(midi_hz(note - 24.0f) * exp2f(p->tune / 12.0f), 20.0f, 10000.0f);
  v->amp_env = 1.0f;
  v->pitch_env = 1.0f;
  v->mod2_phase = noise_bipolar(&v->rng) * 0.12f;   /* Sophie's slight phase variation */
  v->mod3_phase = noise_bipolar(&v->rng) * 0.08f;
  v->model = p->model;
  v->kick = p->kick;
  float decay_s = clampf(p->decay, 0.03f, 4.0f);
  v->amp_mul = expf(logf(0.00012f) / (decay_s * g_fs));
  v->pitch_mul = expf(-1.0f / ((v->kick ? 0.075f : 0.028f) * g_fs));
  v->att_k = one_pole(0.085f);
  v->color = p->color * 0.01f;
  v->metal = p->metal * 0.01f;
  v->feedback = p->feedback * 0.01f;
  v->noise_k = v->kick ? one_pole(0.32f) : one_pole(0.02f + v->color * 0.35f);
  v->sweep_oct = p->sweep * 0.01f * (v->kick ? 2.5f : 4.5f);
  v->r1 = 0.47f + v->color * v->color * 7.6f;
  v->r2 = 1.31f + v->color * 11.17f;
  v->r3 = 2.07f + (1.0f - v->color) * 15.73f;
  float crush = p->crush * 0.01f;
  v->crush_on = crush >= 0.005f;
  v->crush_period = (float)(1 + (int32_t)(crush * crush * 31.0f)) / g_rate;   /* Sophie's hold, in 44.1 kHz samples */
  v->crush_levels = exp2f(15.0f - crush * 12.0f);
  float drive = clampf(p->drive * 0.01f, 0.0f, 1.0f);
  float drive_units = drive <= 0.5f ? drive * 100.0f : 50.0f + 250.0f * powf((drive - 0.5f) * 2.0f, 1.5f);
  v->drive_gain = 1.0f + drive_units * 0.075f;
  float norm = soft_clip(v->drive_gain);
  if (drive > 0.5f) norm += (1.0f - norm) * 0.72f * ((drive - 0.5f) * 2.0f);
  v->drive_inv_norm = 1.0f / norm;
  v->delay_samples = clampf(p->ring_time * 0.001f * g_fs, 1.0f, (float)(SOPH_MAX_DELAY - 2));
  v->tone_k = one_pole(0.02f + p->ring_tone * 0.0098f);
  v->ring_fb = p->ring_feedback * 0.01f;
  v->mix = p->ring_mix * 0.01f;
  float fc = 40.0f * powf(400.0f, p->cutoff * 0.01f);
  v->f = clampf(2.0f * sinf(SOPH_PI * fc / g_fs), 0.001f, 0.95f);
  v->q = clampf(1.0f - p->resonance * 0.009f, 0.05f, 1.0f);
  v->filter_type = p->filter_type;
  v->dj_pos = clampf(p->cutoff * 0.01f, 0.0f, 1.0f);
}

static void soph_note_on(void *st, float note, float vel) {
  sophie_state *s = st;
  note = clampf(note, 0.0f, 127.0f);
  vel = clampf(vel, 0.0f, 1.0f);
  /* a free hit, else the quieter (older) one, faded out from its last sample */
  sophie_hit *v = &s->hit[0];
  if (s->hit[0].active) {
    v = &s->hit[1];
    if (s->hit[1].active && s->hit[0].amp_env < s->hit[1].amp_env) v = &s->hit[0];
  }
  if (v->active) {
    s->declick += v->last;
    s->declick_step = s->declick / (0.002f * g_fs);
  }
  trigger(s, v, note, vel);
}

static void soph_note_off(void *st) { (void)st; }   /* one-shot hits, as Sophie's (the box's AMP has the gate) */

static float render_hit(sophie_hit *v) {
  v->amp_env *= v->amp_mul;
  v->pitch_env *= v->pitch_mul;
  v->attack += (1.0f - v->attack) * v->att_k;
  if (v->amp_env < 0.00012f) { v->active = 0; v->last = 0.0f; return 0.0f; }

  float hz = clampf(v->base_hz * fexp2(v->sweep_oct * v->pitch_env), 18.0f, 18000.0f);
  float inc = SOPH_TAU * hz / g_fs;
  v->carrier_phase = wrap_phase(v->carrier_phase + inc);
  v->mod1_phase = wrap_phase(v->mod1_phase + inc * v->r1);
  v->mod2_phase = wrap_phase(v->mod2_phase + inc * v->r2);
  v->mod3_phase = wrap_phase(v->mod3_phase + inc * v->r3);

  float metal = v->metal, color = v->color, feedback = v->feedback;
  float fb = v->feedback_z * feedback * (2.0f + metal * 10.0f);
  float m1 = fsin(v->mod1_phase + fb);
  float m2 = fsin(v->mod2_phase + m1 * metal * 2.5f);
  float m3 = fsin(v->mod3_phase);
  float index = metal * metal * (2.0f + 24.0f * (0.25f + v->pitch_env * 0.75f));
  float out;
  if (v->kick) {
    /* Sophie's kick: a low sine body owns the pitch and weight, the folded / FM material a quiet texture */
    float body = fsin(v->carrier_phase);
    float folded = fsin(body * (1.0f + color * 2.2f));
    float texture = fsin(v->carrier_phase + m2 * (0.18f + feedback * 1.35f));
    float n = noise_bipolar(&v->rng);
    v->noise_z += (n - v->noise_z) * v->noise_k;
    float click = v->noise_z * v->pitch_env;
    out = body * (0.86f - color * 0.08f) + folded * (0.14f + color * 0.08f) + texture * metal * 0.18f
        + click * (0.012f + metal * 0.035f);
    if (v->model == 1)
      out += 0.16f * fsin(v->carrier_phase * 2.01f + m1 * (0.4f + metal * 2.0f));
    else if (v->model == 2)
      out += 0.14f * fsin(v->carrier_phase * 0.503f - m2 * (0.3f + metal * 1.5f));
    else if (v->model == 3) {
      float stair = __builtin_floorf((v->carrier_phase / SOPH_TAU) * 24.0f) * (SOPH_TAU / 24.0f);
      out += 0.12f * fsin(stair + m1 * (0.5f + metal * 2.0f));
    }
  } else switch (v->model) {
    default:   /* Fuse: two modulators in series, responsive feedback */
      out = fsin(v->carrier_phase + index * (m2 * 0.72f + m1 * 0.28f));
      break;
    case 1:    /* Stack: a dense additive / FM stack */
      out = fsin(v->carrier_phase + index * 0.62f * m1) + 0.55f * fsin(v->carrier_phase * 0.501f + index * 0.35f * m2)
          + 0.32f * fsin(v->carrier_phase * 1.997f + index * 0.22f * m3);
      out *= 0.56f;
      break;
    case 2:    /* Split: parallel operators, asymmetric modulation */
      out = 0.62f * fsin(v->carrier_phase + index * m1) + 0.38f * fsin(v->carrier_phase * 1.003f - index * 0.71f * m2)
          + 0.18f * m3;
      break;
    case 3: {  /* Shard: a stepped wavetable / noise hybrid */
      float stair = __builtin_floorf((v->carrier_phase / SOPH_TAU) * 32.0f) * (SOPH_TAU / 32.0f);
      float n = noise_bipolar(&v->rng);
      v->noise_z += (n - v->noise_z) * v->noise_k;
      out = fsin(stair + index * 0.55f * m1) * (0.85f - metal * 0.25f) + v->noise_z * metal * 0.42f;
      break;
    }
  }
  v->feedback_z = soft_clip(out * 1.4f);
  out *= v->amp_env * v->attack;   /* LEVEL 100 % */

  if (v->crush_on) {
    if (v->crush_count <= 0.0f) { v->crush_hold = out; v->crush_count += v->crush_period; }
    out = v->crush_hold;
    v->crush_count -= 1.0f;
    out = __builtin_floorf(out * v->crush_levels + 0.5f) / v->crush_levels;
  }
  float dry = soft_clip(out * v->drive_gain) * v->drive_inv_norm * v->velocity;

  /* Ring: a short feedback delay (a resonant comb below 10 ms); mix 0 is a bypass but for the buffer write */
  float read = (float)v->delay_pos - v->delay_samples;
  if (read < 0.0f) read += (float)SOPH_MAX_DELAY;
  int32_t r0 = (int32_t)read;
  int32_t rnext = r0 + 1 >= SOPH_MAX_DELAY ? 0 : r0 + 1;
  float frac = read - (float)r0;
  float delayed = v->delay_buf[r0] * (1.0f - frac) + v->delay_buf[rnext] * frac;
  v->delay_lp += v->tone_k * (delayed - v->delay_lp);
  delayed = v->delay_lp;
  v->delay_buf[v->delay_pos] = clampf(dry + delayed * v->ring_fb, -1.0f, 1.0f);
  if (++v->delay_pos >= SOPH_MAX_DELAY) v->delay_pos = 0;
  out = dry * (1.0f - v->mix) + delayed * v->mix;

  /* the filter last, outside the delay's loop, so it can tame what the crusher, drive and ring add */
  v->filter_low += v->f * v->filter_band;
  float high = out - v->filter_low - v->q * v->filter_band;
  v->filter_band += v->f * high;
  switch (v->filter_type) {
    case 0: out = v->filter_low; break;
    case 1: out = high; break;
    case 2: out = high + v->filter_low; break;
    case 4: out = v->filter_band; break;
    default: {   /* DJ: low-pass at the left, band-pass in the middle, high-pass at the right */
      float pos = v->dj_pos;
      if (pos < 0.5f) { float t = pos * 2.0f; out = v->filter_low * (1.0f - t) + v->filter_band * t; }
      else { float t = (pos - 0.5f) * 2.0f; out = v->filter_band * (1.0f - t) + high * t; }
      break;
    }
  }
  if (!(out > -8.0f && out < 8.0f)) { v->active = 0; out = 0.0f; }   /* never NaN: the hit ends */
  v->last = out;
  return out;
}

static inline float flush(float x) { return (x > -1e-20f && x < 1e-20f) ? 0.0f : x; }

static void soph_render(void *st, float *out, int32_t n) {
  sophie_state *s = st;
  for (int32_t i = 0; i < n; ++i) {
    float y = 0.0f;
    for (int32_t h = 0; h < SOPH_HITS; ++h)
      if (s->hit[h].active) y += render_hit(&s->hit[h]);
    if (s->declick != 0.0f) {   /* toward 0 by declick_step a sample */
      y += s->declick;
      float d = s->declick - s->declick_step;
      s->declick = (s->declick_step > 0.0f ? d > 0.0f : d < 0.0f) ? d : 0.0f;
    }
    y *= SOPH_OUT_GAIN;
    out[i] = clampf(y, -1.0f, 1.0f);
  }
  for (int32_t h = 0; h < SOPH_HITS; ++h) {   /* no denormals in the running state */
    sophie_hit *v = &s->hit[h];
    v->filter_low = flush(v->filter_low); v->filter_band = flush(v->filter_band);
    v->delay_lp = flush(v->delay_lp); v->noise_z = flush(v->noise_z); v->feedback_z = flush(v->feedback_z);
    v->pitch_env = flush(v->pitch_env); v->crush_hold = flush(v->crush_hold); v->last = flush(v->last);
  }
  s->declick = flush(s->declick);
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 2u, sizeof(sophie_state), SOPH_PERMILLE,   /* SOPH_PERMILLE: the Makefile's, from tests/sophie_check */
  soph_init, soph_voice_init, soph_note_on, soph_note_off, soph_params, soph_render,
  {0, 0, 0, 0, 0, 0, 0, 0}};
