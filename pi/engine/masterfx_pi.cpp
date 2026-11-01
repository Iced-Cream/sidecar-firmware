// pi/engine/masterfx_pi.cpp - see masterfx_pi.h.
#include "masterfx_pi.h"

#include <string.h>

#include "fx/limiter.h"
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
static float s_fTrim = 1.0f;          // the stem trim, linear
static int s_nTrim = -1;              // ... of this many dB down
static core::fx::Limiter s_Lim;
static float s_fLim = 0.0f, s_fLowWin = 1.0f;   // the limiter's share of 0/1; its lowest gain in this second
static unsigned s_nLook = 0, s_nLowFrames = 0;  // frames in its delay (not 0: it runs); this second's so far
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
  const float trim = (v == 5 || v == 6 ? kStemHotTrim : 1.0f) * s_fTrim;
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
  if (core::master::control.trim != s_nTrim) {   // (the gains' own ramp below smooths it)
    s_nTrim = core::master::control.trim;
    s_fTrim = core::out::Mixer::Trim(-10 * s_nTrim);
  }
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

// SRC MIX: a frame of the stems (at their level in the mix: kStemOnPost) onto l, r, in and out over kFadeFrames. Not clipped
// here: the limiter is the only limit they meet, and they come in only once it is in
static inline void AddStems(float& l, float& r, float t) {
  float x = 0.0f, y = 0.0f;
  if (s_bStPrimed) {
    if (s_nStR != s_nStW) {
      const float* i = s_Stem[s_nStR++ & (kRing - 1)];
      x = i[0], y = i[1];
    } else {
      s_bStPrimed = false, s_nStemUnder = s_nStemUnder + 1;
    }
  }
  s_fStem += t > s_fStem ? 1.0f / kFadeFrames : t < s_fStem ? -1.0f / kFadeFrames : 0.0f;
  s_fStem = s_fStem < 0.0f ? 0.0f : s_fStem > 1.0f ? 1.0f : s_fStem;
  l += x * kStemOnPost * s_fStem, r += y * kStemOnPost * s_fStem;
}

void Process(int32_t (*out)[12], unsigned n) {
  const auto& c = core::master::control;
  // stereo=measure reads the plain return, so all of this stands aside for it whatever BYP says
  const bool on = core::master::on(), meas = core::out::meas::active(), mix = c.src == core::master::kMix;
  const bool chain = on && !c.byp && !meas;
  // The limiter is first in and last out: the stems and the chain's output, neither of them clipped, reach 0/1 only with it
  // fully in, so its crossfade has nothing over full scale on the direct side, and BYP and SRC RET still end bit for bit
  const bool lim = (mix && !meas) || chain || s_fStem > 0.0f || s_fWet > 0.0f;
  const bool run = on || s_fWet > 0.0f, st = mix || s_fStem > 0.0f;
  if (s_bRun && !run) {
    s_bRun = s_bPrimed = false;
    Fr x;   // what is left in the rings: from core 0's side only the way back
    while (s_Out.pop(&x)) {
    }
  }
  if (!lim && s_fLim <= 0.0f) {
    if (s_nLook) s_nLook = 0, s_Lim.Reset(), core::master::lim_gain = 1.0f, s_fLowWin = 1.0f, s_nLowFrames = 0;
    if (!run && !st) return;   // nothing set: no work, not a sample touched
  }
  const bool in = s_fLim >= 1.0f;
  const float ts = mix && !meas && in ? 1.0f : 0.0f, tw = chain && in ? 1.0f : 0.0f, tl = lim ? 1.0f : 0.0f;
  if (st && !s_bStPrimed && s_nStW - s_nStR >= kStemPrime) s_bStPrimed = true;
  if (run) {
    s_bRun = true;
    if (!s_bPrimed && s_Out.size() >= kPrime) s_bPrimed = true;
  }
  float w = s_fWet, u = s_fLim;
  for (unsigned f = 0; f < n; ++f) {
    float l = Fl(out[f][0]), r = Fl(out[f][1]);
    if (st) AddStems(l, r, ts);
    if (run) {
      Fr x{l + kPreOnPost * Fl(out[f][2]), r + kPreOnPost * Fl(out[f][3])};
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
      if (s_bPrimed) w += tw > w ? 1.0f / kFadeFrames : tw < w ? -1.0f / kFadeFrames : 0.0f;
      w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
      if (w > 0.0f) {   // the dry share of both pairs, the wet share on 0/1. At 0 the channels are not written at all
        const float d = 1.0f - w;
        l = l * d + y.l * w, r = r * d + y.r * w;
        out[f][2] = (int32_t)((float)out[f][2] * d), out[f][3] = (int32_t)((float)out[f][3] * d);
      }
    }
    if (lim || u > 0.0f) {   // last on 0/1: what the fade crosses is the direct signal and the same, kLook later
      float a = l, b = r;
      s_Lim.Frame(a, b);
      if (s_nLook < core::fx::Limiter::kLook) ++s_nLook;   // the fade starts with the delay full: no dip
      else u += tl > u ? 1.0f / kLimFade : tl < u ? -1.0f / kLimFade : 0.0f;
      u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
      if (u > 0.0f) {
        const float d = 1.0f - u;
        out[f][0] = Add(0, l * d + a * u), out[f][1] = Add(0, r * d + b * u);
      }
    }
  }
  s_fWet = w, s_fLim = u;
  if (s_nLook) {   // the lowest gain, held for the slow readers: /status and mfx=stats
    const float m = s_Lim.TakeLow();
    if (m < s_fLowWin) s_fLowWin = m;
    if (m < core::master::lim_gain) core::master::lim_gain = m;
    if ((s_nLowFrames += n) >= 48000) core::master::lim_gain = s_fLowWin, s_fLowWin = 1.0f, s_nLowFrames = 0;
  }
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
  unsigned gr = (unsigned)core::master::gr10();
  core::log("mfx: %u frames, core 2 load %u.%03u percent, wet %u %%, underruns %u, drops in %u out %u, stems short %u, limiter %u.%u dB", frames,
            load / 1000, load % 1000, (unsigned)(s_fWet * 100.0f), s_nUnder, s_nDropIn, s_nDropOut, s_nStemUnder, gr / 10, gr % 10);
  return true;
}

Stats Read() { return Stats{s_nFrames, s_nUnder, s_nDropIn, s_nDropOut, s_nStemUnder}; }
float Wet() { return s_fWet; }

void Reset() {
  Fr x;
  while (s_Out.pop(&x)) {   // (s_In is core 2's to empty: a few old frames reach the way back and are crossfaded past)
  }
  s_fWet = s_fStem = s_fLim = 0.0f, s_bPrimed = s_bRun = false;
  s_nStR = s_nStW = 0, s_bStPrimed = s_bGains = false;
  s_Lim.Reset(), s_nLook = s_nLowFrames = 0, s_fLowWin = 1.0f, core::master::lim_gain = 1.0f;
}

}  // namespace masterfx_pi
