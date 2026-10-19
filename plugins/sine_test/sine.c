// plugins/sine_test/sine.c - a test machine: a sine at the trig's note, P1
// TUNE (64 = 0, +-12 semitones), P2 LEVEL, decaying with P3 DECAY; note-off
// releases quickly. Used to test the loader, the slots and the guard.
// Built as an API-1 plugin on purpose (table api 1, init checks the host gives it API 1): the tests' proof
// that API-1 plugins still load and play.
#include <math.h>
#include "rmp/rmp_api.h"

typedef struct {
  float phase, inc, env, decay_k, level, note, tune;
  int gate;
} sine_state;

static const rmp_host *g_host;

static int32_t sine_init(const rmp_host *h) { g_host = h; return h->api == 1u ? 0 : 1; }   /* an API-1 plugin, as installed ones are */
static void sine_voice_init(void *st) { sine_state *s = st; s->level = 0.5f; s->decay_k = 0.9999f; }
static void retune(sine_state *s) {
  float f = 440.0f * powf(2.0f, (s->note + s->tune - 69.0f) / 12.0f);
  s->inc = f / g_host->sample_rate;
}
static void sine_note_on(void *st, float note, float vel) {
  sine_state *s = st; s->note = note; retune(s); s->env = vel; s->gate = 1;
}
static void sine_note_off(void *st) { sine_state *s = st; s->gate = 0; }
static void sine_params(void *st, const float *p) {
  sine_state *s = st;
  s->tune = (p[0] - 64.0f) * (12.0f / 64.0f);
  s->level = p[1] / 127.0f;
  float secs = 0.02f + 4.0f * (p[2] / 127.0f) * (p[2] / 127.0f);
  s->decay_k = expf(-3.0f / (secs * g_host->sample_rate));
  retune(s);
}
static void sine_render(void *st, float *out, int32_t n) {
  sine_state *s = st;
  float k = s->gate ? s->decay_k : 0.995f;
  for (int32_t i = 0; i < n; ++i) {
    out[i] = s->level * s->env * sinf(6.2831853f * s->phase);
    s->phase += s->inc; if (s->phase >= 1.0f) s->phase -= 1.0f;
    s->env *= k;
  }
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, 1u, sizeof(sine_state), 25,   /* API 1 on purpose: the v1 fixture; measured ~17 permille a voice (sinf per sample) */
  sine_init, sine_voice_init, sine_note_on, sine_note_off, sine_params, sine_render, {0}};
