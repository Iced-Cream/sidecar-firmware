// pi/engine/masterfx_pi.cpp - see masterfx_pi.h.
#include "masterfx_pi.h"

#include <string.h>

#include "fx/master.h"
#include "fx/out_measure.h"
#include "fx/out_mix.h"
#include "platform.h"
#include "spsc.h"

namespace masterfx_pi {

struct Fr { float l, r; };
static Spsc<Fr, kRing> s_In, s_Out;
// core 0's
static float s_fWet = 0.0f, s_fStem = 0.0f;
static bool s_bPrimed = false, s_bRun = false;
static float s_Stem[kRing][2];
static float s_Gain[8][2];
static unsigned s_nStR = 0, s_nStW = 0;
static bool s_bStPrimed = false, s_bGains = false;
static volatile uint32_t s_nFrames, s_nUnder, s_nDropIn, s_nDropOut, s_nStemUnder;
static volatile uint64_t s_nCycles;   // core 2's busy ticks (as sendfx_pi's: only ever grows)
static uint64_t s_nLastBusy;          // core 3's (Command): the last mfx=stats
static uint32_t s_nLastFrames, s_nLastMs;

static inline int32_t Sat(int64_t s) { return s > 2147483647 ? 2147483647 : s < -2147483647 - 1 ? -2147483647 - 1 : (int32_t)s; }
static inline int32_t Add(int32_t a, float x) { return Sat((int64_t)a + (int64_t)(x * 2147483648.0f)); }
static inline float Fl(int32_t a) { return (float)a * (1.0f / 2147483648.0f); }

bool WantStems() { return core::master::control.src == core::master::kMix || s_fStem > 0.0f; }

// a stem's gains on L, R, from the Rytm's report: its pan law's linear split, 0 while it is still on main (it sounds there)
static inline void Target(int v, float* t) {
  const auto& c = core::master::control;
  const float trim = v == 5 || v == 6 ? kStemHotTrim : 1.0f;
  if (!c.known) {
    t[0] = t[1] = trim;
  } else if (!((c.off_main >> v) & 1)) {
    t[0] = t[1] = 0.0f;
  } else {
    const int p = c.pan[v];
    const float r = p <= 64 ? (float)p / 64.0f : 1.0f + (float)(p - 64) / 63.0f;   // 0 .. 1 at centre .. 2
    t[0] = (2.0f - r) * trim, t[1] = r * trim;
  }
}

void Stems(const int32_t (*stems)[8], unsigned n) {
  float g[8][2], d[8][2];
  const float lim = 2.0f / (float)kGainFrames * (float)n;   // a block's largest move
  for (int v = 0; v < 8; ++v) {
    float t[2];
    Target(v, t);
    for (int c = 0; c < 2; ++c) {
      if (!s_bGains) s_Gain[v][c] = t[c];
      g[v][c] = s_Gain[v][c];
      float m = t[c] - g[v][c];
      m = m > lim ? lim : m < -lim ? -lim : m;
      s_Gain[v][c] = g[v][c] + m;
      d[v][c] = m / (float)n;
    }
  }
  s_bGains = true;
  for (unsigned f = 0; f < n; ++f) {
    float l = 0.0f, r = 0.0f;
    for (int v = 0; v < 8; ++v) {
      const float x = Fl(stems[f][v]);
      g[v][0] += d[v][0], g[v][1] += d[v][1];
      l += x * g[v][0], r += x * g[v][1];
    }
    if (s_nStW - s_nStR >= kRing) ++s_nStR;   // a backlog: the oldest goes
    float* o = s_Stem[s_nStW++ & (kRing - 1)];
    o[0] = l, o[1] = r;
  }
}

// SRC MIX: the stems (at their level in the mix: kStemOnPost, under the ceiling) onto 0/1, in and out over kFadeFrames
static void AddStems(int32_t (*out)[12], unsigned n) {
  const float t = core::master::control.src == core::master::kMix ? 1.0f : 0.0f;
  if (t == 0.0f && s_fStem <= 0.0f) return;
  if (!s_bStPrimed && s_nStW - s_nStR >= kStemPrime) s_bStPrimed = true;
  for (unsigned f = 0; f < n; ++f) {
    float x = 0.0f, y = 0.0f;
    if (s_bStPrimed) {
      if (s_nStR != s_nStW) {
        const float* i = s_Stem[s_nStR++ & (kRing - 1)];
        x = i[0], y = i[1];
      } else {
        s_bStPrimed = false, s_nStemUnder = s_nStemUnder + 1;
      }
    }
    s_fStem += t > s_fStem ? 1.0f / kFadeFrames : -1.0f / kFadeFrames;
    s_fStem = s_fStem < 0.0f ? 0.0f : s_fStem > 1.0f ? 1.0f : s_fStem;
    out[f][0] = Add(out[f][0], core::out::Mixer::Limit(x * kStemOnPost, core::master::kOutLimit) * s_fStem);
    out[f][1] = Add(out[f][1], core::out::Mixer::Limit(y * kStemOnPost, core::master::kOutLimit) * s_fStem);
  }
}

void Process(int32_t (*out)[12], unsigned n) {
  AddStems(out, n);
  const bool on = core::master::on();
  if (!on && s_fWet <= 0.0f) {   // nothing set: no work, not a sample touched
    if (s_bRun) {
      s_bRun = s_bPrimed = false;
      Fr x;   // what is left in the rings: from core 0's side only the way back
      while (s_Out.pop(&x)) {
      }
    }
    return;
  }
  s_bRun = true;
  // stereo=measure reads the plain return, so the chain stands aside for it whatever BYP says
  const float t = on && !core::master::control.byp && !core::out::meas::active() ? 1.0f : 0.0f;
  if (!s_bPrimed && s_Out.size() >= kPrime) s_bPrimed = true;
  float w = s_fWet;
  for (unsigned f = 0; f < n; ++f) {
    Fr x{Fl(out[f][0]) + kPreOnPost * Fl(out[f][2]), Fl(out[f][1]) + kPreOnPost * Fl(out[f][3])};
    if (!s_In.push(x)) s_nDropIn = s_nDropIn + 1;
    Fr y{0.0f, 0.0f};
    bool have = false;
    if (s_bPrimed) {
      have = s_Out.pop(&y);
      if (!have) {   // ran dry: silence on the wet side, counted, and prime again
        s_nUnder = s_nUnder + 1;
        s_bPrimed = false;
      }
    }
    if (s_bPrimed) w += t > w ? 1.0f / kFadeFrames : t < w ? -1.0f / kFadeFrames : 0.0f;
    w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    if (w > 0.0f) {   // the dry share of both pairs, the wet share on 0/1. At 0 the channels are not written at all
      const float d = 1.0f - w;
      out[f][0] = Add((int32_t)((float)out[f][0] * d), y.l * w), out[f][1] = Add((int32_t)((float)out[f][1] * d), y.r * w);
      out[f][2] = (int32_t)((float)out[f][2] * d), out[f][3] = (int32_t)((float)out[f][3] * d);
    }
  }
  s_fWet = w;
}

void Render() {
  unsigned n = s_In.size();
  if (n == 0) return;
  if (n > kMaxPerRender) n = kMaxPerRender;
  float y[kMaxPerRender][2];
  unsigned k = 0;
  Fr x;
  while (k < n && s_In.pop(&x)) y[k][0] = x.l, y[k][1] = x.r, ++k;
  const uint32_t t0 = core::cycles();
  core::master::render(y, (int)k);
  s_nCycles = s_nCycles + (uint32_t)(core::cycles() - t0);
  for (unsigned i = 0; i < k; ++i)
    if (!s_Out.push(Fr{y[i][0], y[i][1]})) s_nDropOut = s_nDropOut + 1;
  s_nFrames = s_nFrames + k;
}

bool Command(const char* p) {
  if (strcmp(p, "mfx=stats") != 0) return false;
  uint32_t ms = core::now_ms(), dt = ms - s_nLastMs, fr = s_nFrames, frames = fr - s_nLastFrames;
  uint64_t total = s_nCycles, busy = total - s_nLastBusy;
  s_nLastMs = ms, s_nLastFrames = fr, s_nLastBusy = total;
  uint64_t window = (uint64_t)dt * core::cycles_per_second() / 1000;
  unsigned load = window ? (unsigned)(busy * 100000 / window) : 0;   // 1/1000 %
  core::log("mfx: %u frames, core 2 load %u.%03u percent, wet %u %%, underruns %u, drops in %u out %u, stems short %u", frames, load / 1000,
            load % 1000, (unsigned)(s_fWet * 100.0f), s_nUnder, s_nDropIn, s_nDropOut, s_nStemUnder);
  return true;
}

Stats Read() { return Stats{s_nFrames, s_nUnder, s_nDropIn, s_nDropOut, s_nStemUnder}; }
float Wet() { return s_fWet; }

void Reset() {
  Fr x;
  while (s_Out.pop(&x)) {   // (s_In is core 2's to empty: a few old frames reach the way back and are crossfaded past)
  }
  s_fWet = s_fStem = 0.0f, s_bPrimed = s_bRun = false;
  s_nStR = s_nStW = 0, s_bStPrimed = s_bGains = false;
}

}  // namespace masterfx_pi
