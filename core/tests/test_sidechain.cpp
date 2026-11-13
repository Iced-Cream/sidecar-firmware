// core/tests/test_sidechain.cpp - the SIDECHAIN stage of the INSERT chains (core/fx/sidechain.h, insert.h RunSc,
// docs/INSERT-FX.md): PUMP's envelope shape and timing sample for sample, its trig sources (Rytm track, voice, AUDIO IN's
// level onset), DUCK as the INSERT COMP keyed from the source (gain reduction against the key's level, the key HPF, and
// the same answer as a COMP slot when the key is the signal itself), off = untouched, a missing source = no effect, the
// control side (modes, defaults, wanted sources, status, console, json), the mod targets, and the stage's cost.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "check.h"
#include "core.h"
#include "fx/insert.h"
#include "lfo.h"
#include "mod/target.h"
#include "ui/extra.h"
#include "ui/page.h"
#include "ui/remote.h"
#include "ui/ui.h"

using namespace core;
using ins::Engine;

static const float kSr = 48000.0f;
static std::vector<float> g_mem((size_t)ins::kLines * ins::kLine);
static Engine fresh() {
  Engine e(kSr);
  std::fill(g_mem.begin(), g_mem.end(), 0.0f);
  e.Place((float(*)[ins::kLine])g_mem.data());
  e.Reset();
  ins::reset();
  memset((void*)&sc::bus(), 0, sizeof(sc::Bus));
  sc::bus().wanted_trig = ~0u;
  return e;
}
static void setsc(int c, int mode, int src, std::vector<int> p) {
  ins::control.scmode[c] = (uint8_t)mode;
  ins::control.scsrc[c] = (uint8_t)src;
  for (int k = 0; k < sc::kParams; ++k) ins::control.scp[c][k] = (uint8_t)(k < (int)p.size() ? p[k] : 0);
}
// PUMP's numbers, as the design says them (sidechain.h / the note): DPTH linear, ATT / HOLD / REL squared knobs
static int att_n(int v) { float p = v / 127.0f; return (int)(0.03f * p * p * kSr + 0.5f); }
static int hold_n(int v) { float p = v / 127.0f; return (int)(0.25f * p * p * kSr + 0.5f); }
static int rel_n(int v) { float p = v / 127.0f; return (int)((0.01f + 1.49f * p * p) * kSr + 0.5f); }

static const int kB = 16;   // a block = a chunk
// constant `level` through chain 0, `total` frames in blocks of 16; the stems alive (zeros written) unless !alive;
// trigs at the frames in `trigs` (a trig lands at the start of its block)
static std::vector<float> pump(Engine& e, int total, const std::vector<int>& trigs, bool alive = true, float level = 0.5f,
                               int track = 0) {
  std::vector<float> y(total, level);
  float z[kB] = {};
  for (int i = 0; i < total; i += kB) {
    if (alive) sc::write(sc::kTrackStem[track], z, kB);
    for (int t : trigs)
      if (t >= i && t < i + kB) sc::trig_track(track);
    e.ProcessMono(0, &y[i], kB, ins::control);
  }
  return y;
}
static float db(float g) { return 20.0f * std::log10(std::max(g, 1e-9f)); }

int main() {
  // ---- PUMP: instant attack, hold, linear release, sample for sample from the trig's frame
  {
    Engine e = fresh();
    const int D = 127, A = 0, H = 40, R = 30, S = 64, T0 = 480;
    setsc(0, sc::kPump, 0, {D, A, H, R, S, 50});
    CHECK(att_n(A) == 0);
    const int hn = hold_n(H), rn = rel_n(R);
    auto y = pump(e, 12000, {T0});
    for (int i = 0; i < T0; ++i) CHECK(y[i] == 0.5f);                       // nothing before the trig, bit for bit
    for (int j = 0; j < hn; ++j) CHECK(std::fabs(y[T0 + j]) < 1e-6f);        // gmin = 0 from the trig's own frame
    for (int i = 1; i <= rn; ++i) CHECK(std::fabs(y[T0 + hn + i - 1] - 0.5f * (float)i / (float)rn) < 2e-6f);
    for (int i = T0 + hn + rn; i < 12000; ++i) CHECK(y[i] == 0.5f);        // back at unity on the last release frame
    printf("PUMP: instant attack at the trig frame %d, hold %d, linear release %d frames, then unity: exact\n", T0, hn, rn);
    // a depth of 64 (gmin = 1 - 64/127) and a linear attack of attN frames
    e = fresh();
    setsc(0, sc::kPump, 0, {64, 64, 20, 20, 64, 50});
    const int an = att_n(64), h2 = hold_n(20), r2 = rel_n(20);
    const float gmin = 1.0f - 64.0f / 127.0f;
    y = pump(e, 12000, {T0});
    for (int j = 0; j < an; ++j) CHECK(std::fabs(y[T0 + j] - 0.5f * (1.0f - (1.0f - gmin) * (float)(j + 1) / (float)an)) < 2e-6f);
    for (int j = an; j < an + h2; ++j) CHECK(std::fabs(y[T0 + j] - 0.5f * gmin) < 2e-6f);
    CHECK(y[T0 + an + h2 + r2 - 1] == 0.5f);
    printf("PUMP: attack %d frames linear to %.3f, hold %d, release %d: exact\n", an, gmin, h2, r2);
    // SHAP: the release curve u^e (e = 2^((v-64)/32)): below 64 the gain is back sooner than linear, above later
    e = fresh();
    setsc(0, sc::kPump, 0, {127, 0, 0, 60, 20, 50});
    auto fast = pump(e, 20000, {T0});
    e = fresh();
    setsc(0, sc::kPump, 0, {127, 0, 0, 60, 120, 50});
    auto slow = pump(e, 20000, {T0});
    const int rm = rel_n(60);
    CHECK(fast[T0 + rm / 2] > 0.5f * 0.5f + 0.02f && slow[T0 + rm / 2] < 0.5f * 0.5f - 0.02f);
    CHECK(std::fabs(fast[T0 + rm - 1] - 0.5f) < 1e-3f && std::fabs(slow[T0 + rm - 1] - 0.5f) < 1e-3f);
    printf("PUMP SHAP: halfway through the release 0.5 x %.2f (SHAP 20), 0.5 x %.2f (SHAP 120); both at unity at the end\n",
           fast[T0 + rm / 2] / 0.5f, slow[T0 + rm / 2] / 0.5f);
    // a retrigger in the middle of the release goes down from where the gain is (no jump up)
    e = fresh();
    setsc(0, sc::kPump, 0, {127, 0, 20, 60, 64, 50});
    const int t2 = T0 + hold_n(20) + rm / 2 + 3;
    y = pump(e, 20000, {T0, t2});   // the second lands half way through the release
    int t1 = t2 / kB * kB;
    CHECK(y[t1 - 1] > 0.1f && y[t1] <= y[t1 - 1] + 1e-6f);   // it was coming back up
    CHECK(std::fabs(y[t1]) < 1e-6f);   // instant attack: down to gmin again
    printf("PUMP retrigger: restarts the envelope at the next trig\n");
  }

  // ---- PUMP's trig sources: a Rytm track's trig (through its stem being there), a voice's trig, AUDIO IN's level onset
  {
    Engine e = fresh();
    setsc(0, sc::kPump, 3, {127, 0, 20, 20, 64, 50});   // CP: stem 2 (shared with RS), its own trig
    auto y = pump(e, 3000, {960}, true, 0.5f, 3);
    CHECK(y[959] == 0.5f && std::fabs(y[960]) < 1e-6f);
    e = fresh();
    setsc(0, sc::kPump, 2, {127, 0, 20, 20, 64, 50});   // RS shares CP's stem: CP's trig does not fire RS
    y = pump(e, 3000, {960}, true, 0.5f, 3);
    for (float v : y) CHECK(v == 0.5f);
    printf("PUMP: a track's trig fires its own envelope only (RS and CP share a stem, not a trig)\n");
    // a voice's trig
    e = fresh();
    setsc(0, sc::kPump, sc::kV0 + 2, {127, 0, 20, 20, 64, 50});
    std::vector<float> v(3000, 0.5f);
    float z[kB] = {};
    for (int i = 0; i < 3000; i += kB) {
      sc::write(sc::kStems + 2 + 2, z, kB);
      if (i == 960) sc::trig_voice(2);
      e.ProcessMono(0, &v[i], kB, ins::control);
    }
    CHECK(v[959] == 0.5f && std::fabs(v[960]) < 1e-6f);
    printf("PUMP: a voice's trig (V3) fires it\n");
    // AUDIO IN: no trig, a level onset (the key's 80 Hz high pass, a peak follower, THR, a re-arm below half of it)
    e = fresh();
    const int THR = 64, B0 = 4800;
    setsc(0, sc::kPump, sc::kAinL, {127, 0, 20, 20, 64, THR});
    std::vector<float> w(14400, 0.5f), key(kB);
    int first = -1;
    for (int i = 0; i < 14400; i += kB) {
      for (int k = 0; k < kB; ++k) key[k] = i + k >= B0 ? 0.3f * std::sin(2.0f * (float)M_PI * 100.0f * (float)(i + k - B0) / kSr) : 0.0f;
      sc::write(sc::kStems, key.data(), kB);
      e.ProcessMono(0, &w[i], kB, ins::control);
      for (int k = 0; k < kB; ++k)
        if (first < 0 && w[i + k] < 0.49f) first = i + k;
    }
    printf("PUMP from AUDIO IN: the onset at frame %d (the signal starts at %d)\n", first, B0);
    CHECK(first >= B0 && first < B0 + 96);
    for (int i = 0; i < B0; ++i) CHECK(w[i] == 0.5f);
    // one trig for the sustained tone, not one per cycle: back at unity once the envelope has run out, and it stays
    const int run = hold_n(20) + rel_n(20);
    for (int i = first + run + 480; i < 14400; ++i) CHECK(w[i] == 0.5f);
    printf("PUMP from AUDIO IN: one envelope for a sustained tone, none before it\n");
  }

  // ---- DUCK: the INSERT COMP keyed by the source
  {
    // the key is the signal itself (a voice's own pre-chain sound): the same answer as a COMP slot with the same knobs
    const int THR = 70, RAT = 100, SPD = 40;
    Engine e = fresh(), ref = fresh();
    setsc(0, sc::kDuck, sc::kV0, {THR, RAT, SPD, SPD, 0, 0});   // V1's own signal as the key
    ins::control.type[1][0] = ins::kComp;
    for (int k = 0; k < 4; ++k) ins::control.play[1][0][k] = (uint8_t)(k == 0 ? THR : k == 1 ? RAT : k == 2 ? SPD : 0);
    const int N = 24000;
    std::vector<float> a(N), b(N), x(N);
    for (int i = 0; i < N; ++i) {   // a 4 Hz square of loud / quiet bursts of a 220 Hz tone
      float amp = ((i / 6000) & 1) ? 0.04f : 0.7f;
      x[i] = a[i] = b[i] = amp * std::sin(2.0f * (float)M_PI * 220.0f * (float)i / kSr);
    }
    for (int i = 0; i < N; i += kB) {
      sc::write(sc::kStems + 2, &x[i], kB);
      e.ProcessMono(0, &a[i], kB, ins::control);
      ref.ProcessMono(1, &b[i], kB, ins::control);
    }
    float worst = 0.0f, ref_pk = 0.0f;
    for (int i = 2000; i < N; ++i) worst = std::max(worst, std::fabs(a[i] - b[i])), ref_pk = std::max(ref_pk, std::fabs(b[i]));
    printf("DUCK keyed by the signal itself vs a COMP slot: worst difference %.2e (peak %.2f)\n", worst, ref_pk);
    CHECK(worst < 3e-4f);
    // and it really reduced the loud bursts (COMP's own gain reduction)
    float gl = 0.0f, gq = 0.0f;
    for (int i = 5000; i < 6000; ++i) gl = std::max(gl, std::fabs(a[i]));
    for (int i = 11000; i < 12000; ++i) gq = std::max(gq, std::fabs(a[i]));
    CHECK(gl < 0.7f * 0.5f && gq <= 0.04f * 1.001f);   // the loud burst came down a lot, the quiet one is not louder
  }
  {
    // gain reduction against the key's level: a quiet key under the threshold leaves the signal alone, a loud one ducks it
    auto gr_db = [](float key_amp, float key_hz, int hpf) {
      Engine e = fresh();
      setsc(0, sc::kDuck, 0, {64, 100, 10, 10, hpf, 0});   // BD's stem, THR -20 dB
      std::vector<float> y(9600), k(kB);
      for (int i = 0; i < 9600; ++i) y[i] = 0.2f * std::sin(2.0f * (float)M_PI * 3000.0f * (float)i / kSr);
      std::vector<float> x = y;
      for (int i = 0; i < 9600; i += kB) {
        for (int j = 0; j < kB; ++j) k[j] = key_amp * std::sin(2.0f * (float)M_PI * key_hz * (float)(i + j) / kSr);
        sc::write(0, k.data(), kB);
        e.ProcessMono(0, &y[i], kB, ins::control);
      }
      double sx = 0, sy = 0;
      for (int i = 4800; i < 9600; ++i) sx += x[i] * x[i], sy += y[i] * y[i];
      return db((float)std::sqrt(sy / sx));
    };
    float quiet = gr_db(0.02f, 1000, 0), mid = gr_db(0.2f, 1000, 0), loud = gr_db(0.8f, 1000, 0);
    printf("DUCK gain against the key's level (THR -20 dB, RAT 100): key -34 dB %.2f dB, -14 dB %.2f dB, -2 dB %.2f dB\n", quiet, mid, loud);
    CHECK(quiet > -0.1f && mid < -2.0f && loud < mid - 4.0f);
    // the key HPF (126 = 1 kHz+): a 50 Hz key stops driving it, a 5 kHz key still does
    float lo_off = gr_db(0.8f, 50, 0), lo_on = gr_db(0.8f, 50, 100), hi_on = gr_db(0.8f, 5000, 100);
    printf("DUCK key HPF: 50 Hz key %.2f dB off, %.2f dB with HPF; 5 kHz key with HPF %.2f dB\n", lo_off, lo_on, hi_on);
    CHECK(lo_off < -6.0f && lo_on > -0.5f && hi_on < -6.0f);
  }

  // ---- off: untouched, and no work
  {
    Engine e = fresh();
    ins::control.type[0][0] = ins::kDrive;
    for (int k = 0; k < 4; ++k) ins::control.play[0][0][k] = (uint8_t)(k == 0 ? 60 : k == 3 ? 127 : 20);
    std::vector<float> x(4800), a, b;
    for (int i = 0; i < 4800; ++i) x[i] = 0.3f * std::sin(2.0f * (float)M_PI * 330.0f * (float)i / kSr);
    a = x;
    for (int i = 0; i < 4800; i += 12) e.ProcessMono(0, &a[i], 12, ins::control);
    // the stage's settings are in the control block but the mode is OFF: the same bits
    for (int k = 0; k < sc::kParams; ++k) ins::control.scp[0][k] = 127;
    ins::control.scsrc[0] = 5;
    Engine e2 = fresh();
    ins::control.type[0][0] = ins::kDrive;
    for (int k = 0; k < 4; ++k) ins::control.play[0][0][k] = (uint8_t)(k == 0 ? 60 : k == 3 ? 127 : 20);
    for (int k = 0; k < sc::kParams; ++k) ins::control.scp[0][k] = 127;
    b = x;
    for (int i = 0; i < 4800; i += 12) e2.ProcessMono(0, &b[i], 12, ins::control);
    CHECK(memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
    Engine e3 = fresh();
    CHECK(!e3.Active(0, ins::control));
    setsc(0, sc::kDuck, 0, {50, 70, 10, 10, 0, 0});
    CHECK(e3.Active(0, ins::control));
    printf("off: a chain with the stage off is bit for bit what it was, and not active\n");
    // engaged and released: the fade ends idle (no work, untouched again)
    std::vector<float> z(2400, 0.4f);
    for (int i = 0; i < 2400; i += kB) e3.ProcessMono(0, &z[i], kB, ins::control);
    ins::control.scmode[0] = sc::kOff;
    for (int i = 0; i < 2400; i += kB) e3.ProcessMono(0, &z[i], kB, ins::control);
    CHECK(!e3.Active(0, ins::control));
  }

  // ---- a missing source does nothing (stems exist only in Overbridge mode)
  {
    Engine e = fresh();
    setsc(0, sc::kPump, 0, {127, 0, 20, 20, 64, 50});
    auto y = pump(e, 6000, {960, 2000, 3000}, false);   // trigs, but the stem is not there
    for (float v : y) CHECK(v == 0.5f);
    e = fresh();
    setsc(0, sc::kDuck, 0, {0, 127, 0, 0, 0, 0});   // THR 0 dB.. a key never written
    y = pump(e, 6000, {});
    for (float v : y) CHECK(v == 0.5f);
    // the stem stops mid-way: a stale ring is a missing one (its last frames do not keep ducking)
    e = fresh();
    setsc(0, sc::kDuck, 0, {127, 127, 0, 0, 0, 0});   // THR -40 dB
    std::vector<float> loud(kB, 0.9f), s(30000, 0.3f);
    for (int i = 0; i < 30000; i += kB) {
      if (i < 3000) sc::write(0, loud.data(), kB);
      e.ProcessMono(0, &s[i], kB, ins::control);
    }
    CHECK(s[2900] < 0.3f * 0.9f);                                  // ducked while it played
    CHECK(std::fabs(s[29999] - 0.3f) < 1e-5f);                     // and released once it stopped
    printf("a missing source: no effect (PUMP with no stem, DUCK with no key, a stem that stops)\n");
  }

  // ---- the control side
  {
    Engine e = fresh();
    ins::set_sc_mode(3, sc::kPump);
    CHECK(ins::sc_mode(3) == sc::kPump && ins::sc_src(3) == 0 && ins::sc_param(3, 0) == sc::def(sc::kPump, 0));
    CHECK(sc::bus().wanted == 1u && sc::bus().wanted_trig == 1u);   // BD: its stem and its trig
    ins::set_sc_src(3, sc::kAinLR);
    CHECK(sc::bus().wanted == (3u << sc::kStems) && sc::bus().wanted_trig == 0u);   // AUDIO IN L + R, no trig
    ins::set_sc_src(3, sc::kV0 + 4);
    CHECK(sc::bus().wanted == 1u << (sc::kStems + 2 + 4) && sc::bus().wanted_trig == 1u << (sc::kTracks + 4));
    ins::set_sc_param(3, 0, 70);
    ins::set_sc_mode(3, sc::kDuck, true);   // keep: the knobs stay
    CHECK(ins::sc_param(3, 0) == 70 && ins::sc_mode(3) == sc::kDuck);
    ins::set_sc_mode(3, sc::kDuck);          // a plain choice loads the defaults
    ins::set_sc_mode(3, sc::kPump);
    CHECK(ins::sc_param(3, 0) == sc::def(sc::kPump, 0));
    ins::set_sc_mode(3, sc::kOff);
    CHECK(sc::bus().wanted == 0u && !ins::sc_on(3) && !ins::sc_any_on());
    // the status: why it does nothing
    char why[16];
    ins::set_sc_mode(2, sc::kDuck);
    CHECK(!ins::sc_status(2, why, sizeof why) && !strcmp(why, "NO STEMS"));
    ins::set_sc_src(2, sc::kAinL);
    CHECK(!ins::sc_status(2, why, sizeof why) && !strcmp(why, "NO AUDIO IN"));
    float z[kB] = {};
    sc::write(sc::kStems, z, kB);
    CHECK(ins::sc_status(2, why, sizeof why) && !strcmp(why, "OK"));
    ins::set_sc_src(2, sc::kV0 + 1);
    CHECK(ins::sc_status(2, why, sizeof why));
    ins::set_sc_mode(2, sc::kOff);
    CHECK(ins::sc_status(2, why, sizeof why) && !strcmp(why, "OFF"));
    // names and texts
    CHECK(sc::src_from_name("cb") == 11 && sc::src_from_name("ain") == sc::kAinLR && sc::src_from_name("v8") == sc::kV0 + 7 &&
          sc::src_from_name("nope") < 0);
    CHECK(sc::mode_from_name("Pump") == sc::kPump && sc::mode_from_name("duck") == sc::kDuck && sc::mode_from_name("x") < 0);
    char t[16];
    ins::sc_value_text(sc::kDuck, 0, 127, t, sizeof t);
    CHECK(!strcmp(t, "-40DB"));
    ins::sc_value_text(sc::kPump, 1, 0, t, sizeof t);
    CHECK(!strcmp(t, "INST"));
    ins::sc_value_text(sc::kDuck, 4, 0, t, sizeof t);
    CHECK(!strcmp(t, "OFF"));
    // the console and the json
    char o[400];
    CHECK(ins::command("ins=V2:sc:mode:pump", o, sizeof o) && ins::sc_mode(1) == sc::kPump);
    CHECK(ins::command("ins=V2:sc:src:ch", o, sizeof o) && ins::sc_src(1) == 8);
    CHECK(ins::command("ins=V2:sc:p1:77", o, sizeof o) && ins::sc_param(1, 0) == 77);
    CHECK(ins::command("ins=b3:sc:mode:duck", o, sizeof o) && ins::sc_mode(ins::kBus0 + 2) == sc::kDuck);
    CHECK(ins::command("ins=M2:sc:src:ain", o, sizeof o) && ins::sc_src(ins::kM0 + 1) == sc::kAinLR);
    static char js[16384];
    int n = ins::json(js, sizeof js);
    CHECK(n > 0 && strstr(js, "\"sc\":[") && strstr(js, "\"m\":\"PUMP\"") && strstr(js, "\"s\":\"CH\""));
    CHECK(ins::command("ins=V2:sc:off", o, sizeof o) && ins::sc_mode(1) == sc::kOff);
    printf("control: modes, defaults, wanted sources, status, console, json\n");
    ins::reset();
    CHECK(!ins::sc_any_on() && sc::bus().wanted == 0u);
  }

  // ---- the mod targets: kIns params 12..17 (I3:S1 .. S6), an offset on the knob, LFO names by mode
  {
    Engine e = fresh();
    ins::set_sc_mode(2, sc::kPump);
    const mod::Tid t = mod::tid(mod::kIns, 2, 12);
    CHECK(mod::valid(t) && mod::valid(mod::tid(mod::kIns, 12, 17)) && !mod::valid(mod::tid(mod::kIns, 2, 18)));
    char nm[16];
    mod::name(t, nm, sizeof nm);
    CHECK(!strcmp(nm, "I3:S1"));
    CHECK(!strcmp(mod::name(mod::tid(mod::kIns, 12, 17), nm, sizeof nm), "M2:S6"));
    mod::Tid back = 0;
    CHECK(mod::parse("I3:S4", &back) && back == mod::tid(mod::kIns, 2, 15));
    const mod::Range r = mod::range(t);
    CHECK(r.lo == -127 && r.hi == 127);
    const int base = ins::sc_param(2, 0);
    mod::put(t, 20.0f);
    CHECK(ins::sc_param(2, 0) == base && ins::sc_playing(2, 0) == std::min(127, base + 20));
    CHECK(ins::control.scp[2][0] == std::min(127, base + 20));
    mod::put(t, -127.0f);
    CHECK(ins::sc_playing(2, 0) == 0);
    mod::put(t, 0.0f);
    CHECK(mod::knob_now(t) == (float)base);
    char pl[8];
    mod::param_label(t, pl, sizeof pl);
    CHECK(!strcmp(pl, "SDPT") || !strcmp(pl, "SDPTH"));
    CHECK(lfo::params_of(lfo::kTIns + 2) == 18);
    ins::set_sc_mode(2, sc::kDuck);
    mod::param_label(t, pl, sizeof pl);
    CHECK(!strcmp(pl, "STHR"));
    printf("mod: I3:S1..S6 (kIns 12..17) are targets, offsets on the knob, labels follow the mode\n");
    ins::reset();
  }

  // ---- the Rytm's pages: DOWN on the INSERT page's EQ view (and the MASTER FX page's) opens the SIDECHAIN view
  {
    ins::reset();
    int ki = -1, km = -1;
    for (int i = 0; i < ui::extra_pages(); ++i) {
      if (!strcmp(ui::extra_page(i).title, "INSERT")) ki = i;
      if (!strcmp(ui::extra_page(i).title, "MASTER")) km = i;
    }
    CHECK(ki >= 0 && km >= 0);
    ui::ins_page_set(false, 0, 0, 0);
    ui::Canvas c;
    CHECK(ui::extra_page(ki).key(0x41));          // RIGHT: the EQ view
    CHECK(ui::extra_page(ki).key(0x43));          // DOWN: the SIDECHAIN view
    ui::extra_page(ki).enc(0, 1, false);          // A MODE: OFF -> PUMP
    CHECK(ins::sc_mode(0) == sc::kPump);
    ui::extra_page(ki).enc(1, 3, false);          // B SRC: BD -> a few along
    CHECK(ins::sc_src(0) > 0);
    ui::extra_page(ki).enc(2, 10, false);         // C DPTH
    CHECK(ins::sc_param(0, 0) == sc::def(sc::kPump, 0) + 10);
    ui::extra_page(ki).enc(7, 5, true);           // H THR, FUNC: by 8
    CHECK(ins::sc_param(0, 5) == sc::def(sc::kPump, 5) + 40);
    CHECK(ui::remote_render(6 + ki, 0, nullptr, c));   // renders; the text-fit check (check.h) covers the strings
    CHECK(ui::extra_page(ki).key(0x44));          // YES: bus 1
    ui::extra_page(ki).enc(0, 2, false);          // DUCK on B1
    CHECK(ins::sc_mode(ins::kBus0) == sc::kDuck);
    CHECK(ui::remote_render(6 + ki, 0, nullptr, c));
    CHECK(ui::extra_page(ki).key(0x40));          // LEFT: the EQ view again
    CHECK(ui::extra_page(ki).key(0x45));          // NO: the track view
    ui::mfx_page_set(0, true);
    CHECK(ui::extra_page(km).key(0x43));
    ui::extra_page(km).enc(0, 1, false);
    CHECK(ins::sc_mode(ins::kM0) == sc::kPump);
    CHECK(ui::extra_page(km).key(0x44));          // YES: M2
    ui::extra_page(km).enc(0, 2, false);
    CHECK(ins::sc_mode(ins::kM0 + 1) == sc::kDuck);
    CHECK(ui::remote_render(6 + km, 0, nullptr, c));
    CHECK(ui::extra_page(km).key(0x45));
    ins::reset();
    printf("pages: the SIDECHAIN view on the INSERT and MASTER pages (DOWN on the EQ view)\n");
  }

  // ---- the cost: a stereo bus chain, 16-frame chunks (what the bus and master chains see), and a mono voice chain
  {
    auto cost = [](int mode, bool stereo) {
      Engine e = fresh();
      setsc(8, mode, 0, mode == sc::kPump ? std::vector<int>{100, 5, 30, 60, 40, 50} : std::vector<int>{60, 80, 30, 50, 60, 0});
      std::vector<float> key(kB), l(kB), r(kB);
      const int N = 60000;
      uint64_t ns = 0;
      for (int it = 0; it < N; ++it) {
        for (int j = 0; j < kB; ++j) key[j] = 0.5f * std::sin(0.05f * (float)(it * kB + j)), l[j] = r[j] = 0.3f * key[j];
        sc::write(0, key.data(), kB);
        if (mode == sc::kPump && it % 4000 == 0) sc::trig_track(0);
        float y[kB][2];
        for (int j = 0; j < kB; ++j) y[j][0] = l[j], y[j][1] = r[j];
        auto t0 = std::chrono::steady_clock::now();
        if (stereo) e.ProcessStereo(8, y, kB, ins::control);
        else e.ProcessMono(8, l.data(), kB, ins::control);
        ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
      }
      return (double)ns / N;
    };
    for (int m = sc::kPump; m <= sc::kDuck; ++m)
      for (int st = 0; st < 2; ++st) {
        double ns = cost(m, st);
        printf("CPU: %s %s, %.0f ns per 16-frame chunk = %.3f %% of a core at 48 kHz (this Mac, -O2; the Pi is ~3-5x slower)\n",
               sc::mode_name(m), st ? "stereo" : "mono", ns, ns * 3000.0 / 1e9 * 100.0);
      }
  }
  printf("test_sidechain: ALL OK\n");
  return 0;
}
