// plugins/state_test/state.c - sine_test with a 20000-byte per-voice state (the sine's fields + padding):
// more than the 16384 B a voice has in its own engine memory, so it loads only where PSRAM serves the
// state (docs/plugins.md section 8: the Monday check). A sine at the trig's note, P1
// TUNE (64 = 0, +-12 semitones), P2 LEVEL, decaying with P3 DECAY; note-off
// releases quickly. Used to test the loader, the slots and the guard.
#include <math.h>
#include "rmp/rmp_api.h"

typedef struct {
  float phase, inc, env, decay_k, level, note, tune;
  int gate;
  unsigned char pad[20000 - 32];   /* the state is 20000 B; the padding is never used */
} sine_state;

static const rmp_host *g_host;

static int32_t state_init(const rmp_host *h) { g_host = h; return h->api == RMP_API ? 0 : 1; }
static void state_voice_init(void *st) { sine_state *s = st; s->level = 0.5f; s->decay_k = 0.9999f; }
static void retune(sine_state *s) {
  float f = 440.0f * powf(2.0f, (s->note + s->tune - 69.0f) / 12.0f);
  s->inc = f / g_host->sample_rate;
}
static void state_note_on(void *st, float note, float vel) {
  sine_state *s = st; s->note = note; retune(s); s->env = vel; s->gate = 1;
}
static void state_note_off(void *st) { sine_state *s = st; s->gate = 0; }
static void state_params(void *st, const float *p) {
  sine_state *s = st;
  s->tune = (p[0] - 64.0f) * (12.0f / 64.0f);
  s->level = p[1] / 127.0f;
  float secs = 0.02f + 4.0f * (p[2] / 127.0f) * (p[2] / 127.0f);
  s->decay_k = expf(-3.0f / (secs * g_host->sample_rate));
  retune(s);
}
static void state_render(void *st, float *out, int32_t n) {
  sine_state *s = st;
  float k = s->gate ? s->decay_k : 0.995f;
  for (int32_t i = 0; i < n; ++i) {
    out[i] = s->level * s->env * sinf(6.2831853f * s->phase);
    s->phase += s->inc; if (s->phase >= 1.0f) s->phase -= 1.0f;
    s->env *= k;
  }
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, RMP_API, sizeof(sine_state), 25,   /* measured ~17 permille a voice (sinf per sample) */
  state_init, state_voice_init, state_note_on, state_note_off, state_params, state_render, {0}};
