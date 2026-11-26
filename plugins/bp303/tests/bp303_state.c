// plugins/bp303/tests/bp303_state.c - checks of BP303 that need the voice's state: bp303.c compiled in (the Mac build).
//   subnormal  no float or double of the state is ever subnormal (the Pi does not flush them): notes held and released
//              at RES 0, VOL 0, DEC 0, the defaults and at every knob at 127, looked at after every block, through the
//              release, the run-on and the sleep
//   blocks     1-frame render calls give the same samples as 12-frame calls
//   handover   VOL 64 > 65 > 64 on a held note: no step larger than twice the steady signal's largest
//   sweepcap   accents in a row at RES 127 charge the accent sweep capacitor (the cutoff rises); none at ACC 0
// MIT, Copyright 2026 trigcondition and contributors.
#define RMP_NATIVE 1
#define BP303_PERMILLE 300
#include "../bp303.c"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); ++g_fail; } } while (0)
static void hlog(const char *m) { (void)m; }
static uint32_t hrand(uint32_t *s) { return *s = *s * 1664525u + 1013904223u; }

#define SUBF(x) (fpclassify(x) == FP_SUBNORMAL)
static const char *subnormal_in(const bp_state *s) {
#define F(m) if (SUBF(s->m)) return #m
  F(cs); F(l0); F(es); F(rs_t); F(ag_t); F(bg_t); F(sc_t); F(gd_t); F(w1); F(w2); F(w3); F(respot);
  F(rs); F(ag); F(bg); F(sc); F(split); F(sco); F(sc_out); F(gd_out); F(l0g); F(esg); F(l0t); F(est);
  F(wmeg); F(wcap); F(wamt); F(c_meg); F(agn); F(k8); F(m_amp); F(m_off); F(note); F(ph); F(inc); F(inc_t);
  F(meg); F(megs); F(rc2); F(amp); F(d1); F(d2); F(hpx); F(hpy); F(fbx); F(fby); F(y1); F(y2); F(y3); F(y4);
  F(apx); F(apy); F(h2x); F(h2y); F(tops); F(n1); F(n2);
#undef F
  for (int i = 0; i < K_N; ++i) if (SUBF(s->k[i])) return "k";
  for (int i = 0; i < 2 * HB4_N; ++i) if (SUBF(s->hb4[i])) return "hb4";
  for (int i = 0; i < 2 * HB2_N; ++i) if (SUBF(s->hb2[i])) return "hb2";
  return 0;
}
static void set(float *p, const float *def, int k, float v) { memcpy(p, def, 16 * sizeof(float)); if (k >= 0) p[k] = v; }

int main(void) {
  rmp_host host = {RMP_API, 48000.0f, 12, hlog, hrand, {0}};
  CHECK(bp_init(&host) == 0, "init");
  static bp_state s;
  float prm[16], out[12];

  // subnormal: 0 the defaults, 1 RES 0, 2 VOL 0, 3 DEC 0, 4 all 127, 5 all 0, 6 RES 0 and CUT 0
  for (int mode = 0; mode < 7; ++mode) {
    memset(&s, 0, sizeof s);
    bp_voice_init(&s);
    set(prm, k_defaults, mode == 1 ? K_RES : mode == 2 ? K_VOL : mode == 3 ? K_DEC : -1, 0.0f);
    if (mode == 4 || mode == 5) for (int i = 0; i < 16; ++i) prm[i] = mode == 4 ? 127.0f : 0.0f;
    if (mode == 6) { prm[K_RES] = 0.0f; prm[K_CUT] = 0.0f; }
    bp_params(&s, prm);
    const char *bad = 0;
    int at = -1;
    // a note held 6 s, released, 3 s (the release, the run-on, asleep), a short accented note, 1 s
    for (int b = 0; b < 10 * 4000 && !bad; ++b) {
      if (b == 0) bp_note_on(&s, 36.0f, 0.5f);
      if (b == 6 * 4000) bp_note_off(&s);
      if (b == 9 * 4000) bp_note_on(&s, 48.0f, 1.0f);
      if (b == 9 * 4000 + 400) bp_note_off(&s);
      bp_render(&s, out, 12);
      bad = subnormal_in(&s);
      at = b;
      for (int i = 0; i < 12; ++i) if (SUBF(out[i])) bad = "out";
    }
    printf("subnormal  mode %d: %s\n", mode, bad ? bad : "none");
    CHECK(!bad, "mode %d: %s is subnormal at block %d", mode, bad, at);
  }

  {   // blocks: n = 1 against n = 12
    static bp_state a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    bp_voice_init(&a); bp_voice_init(&b);
    set(prm, k_defaults, K_RES, 100.0f);
    bp_params(&a, prm); bp_params(&b, prm);
    int diff = 0;
    for (int blk = 0; blk < 4000; ++blk) {
      if (blk % 500 == 0) { bp_note_on(&a, 36.0f + (float)(blk / 500), blk % 1000 ? 1.0f : 0.5f); bp_note_on(&b, 36.0f + (float)(blk / 500), blk % 1000 ? 1.0f : 0.5f); }
      if (blk % 500 == 300 && blk % 1500 != 300) { bp_note_off(&a); bp_note_off(&b); }
      float o1[12];
      bp_render(&a, out, 12);
      for (int i = 0; i < 12; ++i) bp_render(&b, o1 + i, 1);
      diff += memcmp(out, o1, sizeof out) != 0;
    }
    printf("blocks     1-frame against 12-frame calls: %d of 4000 blocks differ\n", diff);
    CHECK(diff == 0, "%d blocks differ", diff);
  }

  {   // handover: VOL 64 > 65 > 64 on a held note
    memset(&s, 0, sizeof s);
    bp_voice_init(&s);
    set(prm, k_defaults, K_VOL, 64.0f);
    prm[K_DEC] = 127.0f;
    bp_params(&s, prm);
    bp_note_on(&s, 36.0f, 0.5f);
    float last = 0.0f, steady = 0.0f, cross = 0.0f;
    for (int blk = 0; blk < 4000; ++blk) {
      if (blk == 2000) { prm[K_VOL] = 65.0f; bp_params(&s, prm); }
      if (blk == 3000) { prm[K_VOL] = 64.0f; bp_params(&s, prm); }
      bp_render(&s, out, 12);
      for (int i = 0; i < 12; ++i) {
        float d = fabsf(out[i] - last);
        last = out[i];
        if (blk >= 1500 && blk < 2000 && d > steady) steady = d;
        if (((blk >= 2000 && blk < 2100) || (blk >= 3000 && blk < 3100)) && d > cross) cross = d;
      }
    }
    printf("handover   largest sample step: steady %.4f, across VOL 64 / 65 %.4f\n", steady, cross);
    CHECK(steady > 0.0f && cross < 2.0f * steady, "a step of %.4f across the hand-over (steady %.4f)", cross, steady);
  }

  // sweepcap: six accents in a row, RES 127 (the capacitor path): the cutoff's lift after the sixth against the first
  for (int acc = 0; acc < 2; ++acc) {
    memset(&s, 0, sizeof s);
    bp_voice_init(&s);
    set(prm, k_defaults, K_RES, 127.0f);
    prm[K_ACC] = acc ? 127.0f : 0.0f;
    bp_params(&s, prm);
    float lift1 = 0.0f, lift6 = 0.0f;
    for (int blk = 0; blk < 6 * 480; ++blk) {
      if (blk % 480 == 0) bp_note_on(&s, 36.0f, 1.0f);
      if (blk % 480 == 280) bp_note_off(&s);
      bp_render(&s, out, 12);
      float lift = s.l0t - s.l0;
      if (blk < 480 && lift > lift1) lift1 = lift;
      if (blk >= 5 * 480 && lift > lift6) lift6 = lift;
    }
    printf("sweepcap   ACC %d: the sweep's lift, octaves: first accent %.3f, sixth %.3f\n", acc ? 127 : 0, lift1, lift6);
    if (acc) CHECK(lift1 > 0.2f && lift6 > 1.1f * lift1, "the capacitor does not build up (%.3f, %.3f)", lift1, lift6);
    else CHECK(lift1 == 0.0f && lift6 == 0.0f, "ACC 0: the sweep moves the cutoff (%.3f, %.3f)", lift1, lift6);
  }

  printf(g_fail ? "bp303_state: %d FAILED\n" : "bp303_state: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
