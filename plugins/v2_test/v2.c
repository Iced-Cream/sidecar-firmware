// plugins/v2_test/v2.c - a test machine for plugin API 2 (sim/tests/test_rmp.py, sim/rmp_e2e.py).
//   P1 PRE (the preset knob), P2 LEVEL and P3 TONE (role preset), P4 DETUNE (role offset, 64 = 0,
//   +-1 semitone), P5 SMP (a pool slot, 0 OFF), P6 MODE (SINE, POOL in two zones), P7 JUMP (POOL: a
//   trig starts at (JUMP - 64) x 1000 frames when JUMP >= 64, else at 0), P8 GAIN (free), P9 ROOT
//   (note text), P10 SPARE (range 10..20). TONE, GAIN, ROOT and SPARE do not change the sound: they are
//   there for the knob kinds, roles and ranges the Rytm shows.
//   SINE: a sine at the trig's note + DETUNE + the native preset's cents, level LEVEL/127.
//   POOL: the pool sample from the playhead, its first channel, level LEVEL/127; while pool_info is not
//   ready the voice is silent and the playhead waits (it moves by the frames delivered).
//   load_entry (native presets, extension .v2p): "V2P <level> <tone> <cents>" - three numbers; values
//   P2 = level, P3 = tone; the voice's hidden detune = cents - 100 (0..200 -> -100..+100 cents).
//   voice_enter / voice_leave: counted (the state's enters; v2_leaves for all voices, read by the tests);
//   voice_leave in POOL mode also calls pool_info for its slot (a host must not prepare it then).
#include <math.h>
#include "rmp/rmp_api.h"

typedef struct {
  float phase, inc, env, note, level, offs, cents;
  int32_t gate, mode, smp, jump;
  uint32_t playhead, enters, pool_waits;
} v2_state;

static const rmp_host *g_host;
#ifdef RMP_NATIVE
__attribute__((visibility("default")))
#endif
uint32_t v2_leaves;   /* voice_leave calls, all voices (image-wide: tests only) */

static void retune(v2_state *s) {
  float f = 440.0f * powf(2.0f, (s->note + s->offs + s->cents / 100.0f - 69.0f) / 12.0f);
  s->inc = f / g_host->sample_rate;
}
static int32_t v2_init(const rmp_host *h) { g_host = h; return h->api >= 2u ? 0 : 1; }
static void v2_voice_init(void *st) { v2_state *s = st; s->level = 100.0f / 127.0f; }
static void v2_voice_enter(void *st) { ((v2_state *)st)->enters++; }
static void v2_voice_leave(void *st) {
  v2_state *s = st;
  ++v2_leaves;
  if (s->mode == 1 && s->smp > 0) {   /* asks for its pool slot while leaving: the host must answer EMPTY (tests) */
    rmp_pool_info_fn info = rmp_pool_info_of(g_host);
    rmp_pool_info pi;
    if (info) info(s->smp, &pi);
  }
}
static void v2_note_on(void *st, float note, float vel) {
  v2_state *s = st;
  s->note = note; s->env = vel; s->gate = 1; s->phase = 0.0f;
  s->playhead = s->jump >= 64 ? (uint32_t)(s->jump - 64) * 1000u : 0u;
  retune(s);
}
static void v2_note_off(void *st) { ((v2_state *)st)->gate = 0; }
static void v2_params(void *st, const float *p) {
  v2_state *s = st;
  s->level = p[1] / 127.0f;
  s->offs = (p[3] - 64.0f) / 64.0f;
  s->smp = (int32_t)p[4];
  s->mode = ((int32_t)p[5] * 2) >> 7;
  s->jump = (int32_t)p[6];
  retune(s);
}
static void v2_render(void *st, float *out, int32_t n) {
  v2_state *s = st;
  if (s->mode == 1) {
    rmp_pool_info_fn info = rmp_pool_info_of(g_host);
    rmp_pool_read_fn rd = rmp_pool_read_of(g_host);
    rmp_pool_info pi;
    float b[2 * 16];
    int32_t got = 0, ch = 1;
    if (info && rd && s->smp > 0 && info(s->smp, &pi) == RMP_POOL_OK) {
      ch = (int32_t)pi.channels;
      got = rd(s->smp, s->playhead, b, n > 16 ? 16 : n);
      s->playhead += (uint32_t)got;
    } else if (s->smp > 0) {
      s->pool_waits++;
    }
    for (int32_t i = 0; i < n; ++i) out[i] = i < got ? s->level * b[i * ch] : 0.0f;
    return;
  }
  float k = s->gate ? 0.99999f : 0.995f;
  for (int32_t i = 0; i < n; ++i) {
    out[i] = s->level * s->env * sinf(6.2831853f * s->phase);
    s->phase += s->inc;
    if (s->phase >= 1.0f) s->phase -= 1.0f;
    s->env *= k;
  }
}
static int32_t v2_load_entry(void *st, const uint8_t *bytes, int32_t len, int16_t values[16]) {
  v2_state *s = st;
  int32_t v[3] = {0, 0, 0}, nv = 0, in = 0;
  if (len < 3 || bytes[0] != 'V' || bytes[1] != '2' || bytes[2] != 'P') return 1;
  for (int32_t i = 3; i < len; ++i) {
    uint8_t c = bytes[i];
    if (c >= '0' && c <= '9') {
      if (!in) { if (nv == 3) break; in = 1; ++nv; }
      v[nv - 1] = v[nv - 1] * 10 + (c - '0');
    } else {
      in = 0;
    }
  }
  if (nv < 3) return 2;
  values[1] = (int16_t)(v[0] > 127 ? 127 : v[0]);
  values[2] = (int16_t)(v[1] > 127 ? 127 : v[1]);
  s->cents = (float)(v[2] > 200 ? 200 : v[2]) - 100.0f;
  retune(s);
  return 0;
}

RMP_ENTRY const rmp_plugin rmp_entry = {
  RMP_PLUGIN_MAGIC, RMP_API, sizeof(v2_state), 30,   /* a sinf a sample, as sine_test (measured ~17 permille) */
  v2_init, v2_voice_init, v2_note_on, v2_note_off, v2_params, v2_render,
  {0, RMP_FN(v2_load_entry), RMP_FN(v2_voice_enter), RMP_FN(v2_voice_leave), 0, 0, 0, 0}};
