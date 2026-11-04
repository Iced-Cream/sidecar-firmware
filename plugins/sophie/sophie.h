// plugins/sophie/sophie.h - SOPHIE's per-voice state (sophie.c; tests/sophie_check.cpp reads it to scan the state's
// floats for NaN and denormals).
// SPDX-License-Identifier: MIT
// Copyright 2026 trigcondition and contributors. The DSP is Sophie's (Matt Estela, MIT: LICENSE.sophie, NOTICE).
#ifndef SOPHIE_H
#define SOPHIE_H
#include <stdint.h>

#define SOPH_HITS 2           /* hits a state rings at once: the new one and the one before it (its tail) */
#define SOPH_MAX_DELAY 1664   /* the Ring delay's buffer, ~34.7 ms at 48 kHz (Sophie's 1536 at 44.1 kHz) */
#define SOPH_NFLOATS 39       /* the float fields of sophie_hit before its ints */

/* one hit: Sophie's voice, with the patch it was triggered with turned into per-hit constants */
typedef struct {
  /* running (floats first: SOPH_NFLOATS of them) */
  float carrier_phase, mod1_phase, mod2_phase, mod3_phase;
  float amp_env, pitch_env, attack, feedback_z;
  float filter_low, filter_band, noise_z, delay_lp;
  float crush_hold, crush_count, last;
  /* the snapshot, as constants */
  float base_hz, velocity, amp_mul, pitch_mul, att_k, noise_k, sweep_oct;
  float color, metal, feedback, r1, r2, r3;
  float crush_period, crush_levels, drive_gain, drive_inv_norm;
  float delay_samples, tone_k, ring_fb, mix, f, q, dj_pos;
  /* ints */
  int32_t model, kick, filter_type, crush_on, delay_pos, active;
  uint32_t rng;
  int32_t pad_;
  float delay_buf[SOPH_MAX_DELAY];
} sophie_hit;

/* the knobs, in Sophie's own units */
typedef struct {
  float tune, color, metal, feedback, sweep, decay, crush, drive, cutoff, resonance;
  float ring_time, ring_feedback, ring_mix, ring_tone;
  int32_t model, kick, filter_type, pad_;
} sophie_patch;

typedef struct {
  sophie_hit hit[SOPH_HITS];
  sophie_patch patch;
  float declick, declick_step;   /* a stolen hit's last sample, faded out over 2 ms */
  uint32_t rng;
  int32_t pad_;
} sophie_state;

#endif
