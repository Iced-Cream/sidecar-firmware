// core/dsp/grain.h - GRAIN, the DSP library's granular block (2026-11-10; one note of upstream's GRANULAR machine,
// core/granular/granular.cpp render_impl on trigcondition/sidecar-firmware dev, ported as is: Connor 15:44, library
// extensions not specific to a mod). A note's stream of short windowed grains read from a source at a position, with
// its attack / release envelope. In a POLY (poly.h) the copies share the first copy's live ring and grain count, so
// POLY {GRAIN} with VCRV 1, LLEV 60 is upstream's GRANULAR voice; on its own, one note at a time.
//
// Sources (SMP): 0 the block's input (L + R) / 2 - route a stem or AUDIO IN to both sides - recorded into the voice's
// ring (kRing frames, 5.5 s, float) while not frozen, plus the feedback (the voice's own output of the last block);
// 1..125 a pool entry n - 1 (core/samples; int16, a stereo one summed as upstream's mono_at, its own rate); 126 PAD,
// 127 BELLS (upstream's built-ins, generated at dsp::begin, 4 s each, pitched at C4). A note sets the pitch (60 = the
// source's own).
//
// Params (14 bits; the voice scale, value 0..127.98):
//   P0 LEV   level, value / 100 (unity at 100; upstream: none)
//   P1 SMP   the source, as above
//   P2 POS   a pool entry: where the playhead starts (0 the start .. 127 the end); the input: how far back the grains
//            read (0 the newest .. 127 the oldest the ring holds)
//   P3 SPRY  each grain's position jittered by up to 2 s (squared), its start time by up to +-50 %
//   P4 SIZE  5 ms .. 1 s (exponential)       P5 DENS  1 .. 200 grains a second (exponential)
//   P6 PRND  per-grain random pitch, up to +-12 semitones
//   P7 REV   the probability a grain plays reversed (0 .. 100 %)
//   P8 WIN   zones: HANN TRI TUKEY PERC
//   P9 ATK   P10 REL  the note's envelope, 1 ms .. 10 s (10^(4 v/127) ms; full while held)
//   P11 HOLD 0..119: a pool entry - the playhead's speed, 1x at 0 slowing to 0 at 119; the input - the feedback (0 ..
//            0.95, soft-clipped); 120..127 FREEZE (the playhead stops / the ring stops recording)
//   P12 TUN  semitones from 64       P13 FIN  cents from 64 x 64 (upstream: none; 64 the note as played)
//   P14 HOFS the head moved (8192 none, 0 -1, 16383 +1): a pool entry x a quarter of it (4 s at most), the input x half
//            the ring's room - upstream ANIM's head wave arrives here through a mod point
//   P15 LOFS the input only: further back, value / 127 x half the room (upstream ANIM's own depth)
// ctl(0): the note's envelope as played (smoothed): a POLY's weight. done(): the note has ended.
// Built only by make() into bytes() of zeroed memory (the ring follows the block).
#pragma once
#include "../samples/samples.h"
#include "blocks.h"

namespace core {
namespace dsp {

constexpr uint32_t kGrainBuiltinFrames = 4 * 48000;
constexpr int kGrainWin = 512, kGrainWins = 4;
const int16_t* grain_builtin(int b);   // PAD (0) / BELLS (1): kGrainBuiltinFrames int16 at 48 kHz; null before grain_build
const float* grain_window(int w);      // kGrainWin + 1 floats
void grain_build();                    // control core: the built-ins and the windows, once (dsp::begin)

class Grain : public Block {
 public:
  static constexpr uint32_t kRing = 1u << 18;   // 5.46 s
  static constexpr int kGrains = 64;             // upstream's kDefaultCap: a voice's grains at most, all its notes'
  static constexpr int kHoldFreeze = 120;
  static constexpr size_t kShared = kRing * sizeof(float);   // the ring, after the block (BlockInfo::shared)
  Grain() {
    p_[0] = 100 * 64; p_[1] = 0; p_[2] = 0; p_[3] = 0; p_[4] = 64 * 64; p_[5] = 64 * 64; p_[6] = 0; p_[7] = 0;
    p_[8] = 0; p_[9] = 0; p_[10] = 64 * 64; p_[11] = 0; p_[12] = 64 * 64; p_[13] = 64 * 64; p_[14] = 8192; p_[15] = 0;
    grain_build();
    lead_ = this;
    ring_ = (float*)((uint8_t*)this + head());   // (zero: dsp's PLACE / CHAIN clear a block's memory before make())
  }
  static size_t head() { return (sizeof(Grain) + 63) & ~(size_t)63; }
  static size_t bytes() { return head() + kShared; }   // what make() needs: the block, then its ring
  const char* name() const override { return "GRAIN"; }
  void share(proc::Proc* first) override { lead_ = (Grain*)first; ring_ = nullptr; hosted_ = true; }
  void voice(const proc::Block& b, const float* last) override {
    hosted_ = true;
    voice_part(b, last);
  }
  // a steal or the link: the note's grains fade out (upstream's orphans), the note is over
  void link_reset() override {
    for (G& g : g_) if (g.on) orphan(g, envs_);
    on_ = false;
  }
  void midi(int, uint8_t status, uint8_t d1, uint8_t d2) override {
    const uint8_t st = status & 0xF0;
    if (st == 0x90 && d2) {
      if (on_) for (G& g : g_) if (g.on) orphan(g, envs_);   // a new note on a sounding one: its grains fade
      key_ = d1, on_ = true;
      env_ = envs_ = 0.0f, stage_ = 0, scan_ = 0.0, next_ = 0.0f;
      rnd_ = (seed_ = seed_ * 1664525u + 1013904223u) | 1u;
    } else if ((st == 0x80 || st == 0x90) && on_) {
      stage_ = 2;
    }
  }
  bool done() const override { return !on_; }
  float ctl(int k) const override { return k == 0 && on_ ? envs_ : 0.0f; }

  void process(const proc::Block& b) override {
    const int nfr = b.frames;
    if (!hosted_) voice_part(b, own_last_);
    Grain& L = *lead_;
    const float sr = kRate;
    // the knobs (upstream's set_knobs)
    const int smp = p_[1] / 64;
    const float pos = c01(v(p_[2]) / 127.0f);
    const float s = c01(v(p_[3]) / 127.0f);
    const float spray_s = 2.0f * s * s, spr = s;
    const float size_s = 0.005f * powf(200.0f, c01(v(p_[4]) / 127.0f));
    const float dens = powf(200.0f, c01(v(p_[5]) / 127.0f));
    const float prnd = 12.0f * c01(v(p_[6]) / 127.0f), rev = c01(v(p_[7]) / 127.0f);
    const int win = vzone(p_[8], kGrainWins);
    const float atk = tsec(v(p_[9])), rel = tsec(v(p_[10]));
    const float hv = v(p_[11]);
    const bool freeze = hv >= (float)kHoldFreeze;
    const float hold = freeze ? 1.0f : c01(hv / (float)(kHoldFreeze - 1));
    const float scan_k = freeze ? 0.0f : 1.0f - hold;
    const float tune = (v(p_[12]) - 64.0f) + (v(p_[13]) - 64.0f) / 64.0f;
    const float hofs = ((float)p_[14] - 8192.0f) / 8192.0f, lofs = c01(v(p_[15]) / 127.0f);
    // the source
    samples::View sv{};
    const bool live = smp == 0;
    const int16_t* data = nullptr;
    uint32_t len = 0;
    float srate = 48000.0f;
    int dch = 1;
    if (smp >= 126) data = grain_builtin(smp - 126), len = data ? kGrainBuiltinFrames : 0;
    else if (smp > 0 && samples::get(smp - 1, &sv) && sv.frames > 1) data = sv.pcm, len = sv.frames, srate = sv.rate, dch = sv.ch == 2 ? 2 : 1;
    if (!data) len = 0;
    if (smp != last_smp_) {   // another source: this note's grains on the old one fade out on it
      for (G& g : g_) if (g.on && !g.old) g.old = true, orphan(g, envs_);
      old_ = Old{last_data_, last_len_, last_live_, last_ch_};
      last_smp_ = smp;
    }
    last_data_ = data, last_len_ = len, last_live_ = live, last_ch_ = (uint8_t)dch;
    const float* ring = L.ring_;
    const uint32_t rw = L.ring_w_;
    const uint32_t filled = rw < kRing ? rw : kRing;
    // the note: its envelope this block, its new grains
    float envb[proc::kMaxFrames];
    const float a_inc = 1.0f / (atk * sr), rcoef = expf(-6.9078f / (rel * sr));
    const float size_f = size_s * sr, interval = sr / dens;
    const float ggain = 1.6f / sqrtf(dens * size_s > 1.0f ? dens * size_s : 1.0f);
    bool sounding = on_;
    if (on_) {
      bool fin = false;
      for (int i = 0; i < nfr; ++i) {
        if (stage_ == 0) {
          env_ += a_inc;
          if (env_ >= 1.0f) env_ = 1.0f, stage_ = 1;
        } else if (stage_ == 2) {
          env_ *= rcoef;
          if (env_ < 1e-4f && envs_ < 2e-4f) fin = true;
        }
        envs_ += (env_ - envs_) * kEnvSmooth;
        envb[i] = envs_;
      }
      if (fin) on_ = false, sounding = false;
    }
    if (sounding) {
      while (next_ < (float)nfr) {
        int s0 = (int)next_;
        next_ += interval * (1.0f + (u01(rnd_) - 0.5f) * spr);
        if (stage_ == 2 && env_ < 1e-3f) continue;
        if (!data && !live) continue;
        if (L.ng_ >= kGrains) continue;
        float semis = (float)key_ - 60.0f + tune + (prnd > 0.0f ? (2.0f * u01(rnd_) - 1.0f) * prnd : 0.0f);
        float rate = exp2f(semis / 12.0f) * srate / sr;
        bool r = u01(rnd_) < rev;
        float lsrc = size_f * rate;
        double gp;
        if (live) {
          if ((float)filled < lsrc + size_f + 512.0f) continue;   // not enough recorded yet
          double newest = (double)rw - 3, oldest = (double)rw - filled + 256 + size_f + 64;
          double room = newest - oldest - lsrc;
          if (room < 0) continue;
          double hb = pos + (hofs + lofs) * 0.5;
          hb = hb < 0 ? 0 : hb > 1 ? 1 : hb;
          double d = hb * room + spray_s * sr * u01(rnd_);
          if (d > room) d = room;
          double top = newest - d;
          gp = r ? top : top - lsrc;
        } else {
          double span = 0.25 * len < 4.0 * srate ? 0.25 * len : 4.0 * srate;
          double hc = (double)pos * len + scan_ + hofs * span;
          double c = hc + (spray_s > 0 ? (2 * u01(rnd_) - 1) * spray_s * srate : 0.0f);
          c = fmod(c, (double)len);
          if (c < 0) c += len;
          gp = r ? c + lsrc * 0.5 : c - lsrc * 0.5;
          if ((double)len > lsrc + 4) {
            double lo = r ? lsrc + 1 : 0, hi = r ? len - 2 : len - 2 - lsrc;
            gp = gp < lo ? lo : gp > hi ? hi : gp;
          } else {
            gp = fmod(gp, (double)len);
            if (gp < 0) gp += len;
          }
        }
        int gi = 0;
        while (gi < kGrains && g_[gi].on) ++gi;
        if (gi == kGrains) continue;
        G& g = g_[gi];
        g.pos = gp, g.inc = r ? -rate : rate;
        float gs = size_f < 16 ? 16 : size_f;
        g.wph = 0, g.winc = 1.0f / gs;
        g.ek = 1.0f / (g.winc * (kEdge < 0.5f * gs ? kEdge : 0.5f * gs));
        g.oenv = g.ofade = g.odec = 0;
        g.gain = ggain;
        g.win = (uint8_t)win, g.s0 = (uint16_t)s0, g.on = true, g.old = false;
        ++L.ng_;
      }
      next_ -= (float)nfr;
      if (!live) scan_ += nfr * (double)srate / sr * scan_k;
      if (len && scan_ >= len) scan_ -= len;
    }
    // the grains
    float acc[proc::kMaxFrames];
    for (int i = 0; i < nfr; ++i) acc[i] = 0.0f;
    int n_live = 0;
    for (G& g : g_) {
      if (!g.on) continue;
      if (g.odec <= 0.0f && !sounding) { g.on = false; continue; }   // its note ended (quiet by now)
      bool more;
      if (g.old) more = old_.live ? play<true>(g, ring, nullptr, 0, 1, envb, acc, nfr)
                       : old_.data ? play<false>(g, nullptr, old_.data, old_.len, old_.ch, envb, acc, nfr) : false;
      else if (live) more = play<true>(g, ring, nullptr, 0, 1, envb, acc, nfr);
      else if (data) more = play<false>(g, nullptr, data, len, dch, envb, acc, nfr);
      else more = false;
      if (!more) g.on = false;
      else ++n_live;
    }
    L.ng_next_ += n_live;
    const float lev = v(p_[0]) / 100.0f;
    for (int i = 0; i < nfr; ++i) {
      float y = acc[i] * lev;
      out(b, 0, i, y);
      out(b, 1, i, y);
      if (!hosted_) own_last_[i] = y;
    }
  }

  // tests
  int grains() const { int n = 0; for (const G& g : g_) n += g.on; return n; }
  uint32_t ring_frames() const { return lead_->ring_w_; }

 private:
  static constexpr float kEdge = 96.0f, kOrphan = 144.0f, kEnvSmooth = 0.03f;
  static constexpr uint32_t kSeam = 96;
  struct G {
    double pos;
    float inc, wph, winc, gain, ek, oenv, ofade, odec;
    uint8_t win;
    uint16_t s0;
    bool on, old;
  };
  struct Old { const int16_t* data; uint32_t len; bool live; uint8_t ch; };
  G g_[kGrains] = {};
  Grain* lead_;
  float* ring_;
  bool hosted_ = false;
  // the note
  bool on_ = false;
  uint8_t key_ = 60, stage_ = 0;
  float env_ = 0.0f, envs_ = 0.0f, next_ = 0.0f;
  double scan_ = 0.0;
  uint32_t rnd_ = 1, seed_ = 0x2545f491u;
  int last_smp_ = -1;
  const int16_t* last_data_ = nullptr;
  uint32_t last_len_ = 0;
  bool last_live_ = false;
  uint8_t last_ch_ = 1;
  Old old_ = {};
  // the voice (the lead's)
  uint32_t ring_w_ = 0, seam_in_ = 0;
  bool ring_on_ = false, was_frozen_ = false;
  int ng_ = 0, ng_next_ = 0;
  float own_last_[proc::kMaxFrames] = {};

  static float v(int x) { return (float)x / 64.0f; }
  static float c01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
  static float tsec(float x) { x = x < 0 ? 0 : x > 127 ? 127 : x; return 0.001f * powf(10.0f, x / 127.0f * 4.0f); }
  static uint32_t lcg(uint32_t& s) { return s = s * 1664525u + 1013904223u; }
  static float u01(uint32_t& s) { return (float)(lcg(s) >> 8) * (1.0f / 16777216.0f); }
  static float softr(float x) { x = x < -3 ? -3 : x > 3 ? 3 : x; float x2 = x * x; return x * (27 + x2) / (27 + 9 * x2); }
  void orphan(G& g, float env) {
    if (g.odec > 0) return;
    g.oenv = env, g.ofade = 1.0f, g.odec = 1.0f / kOrphan;
  }
  // a seam in the ring (a thaw, a new source, the first block): the frames before it fade out over kSeam, the new in
  void seam() {
    uint32_t w = ring_w_, n = w < kSeam ? w : kSeam;
    for (uint32_t j = 0; j < n; ++j) {
      float u = (float)(j + 1) / (float)(kSeam + 1);
      ring_[(w - 1 - j) & (kRing - 1)] *= u * u * (3.0f - 2.0f * u);
    }
    seam_in_ = kSeam;
  }
  // the voice's part, once a block (the lead): the grain count; the ring takes the input plus the feedback
  void voice_part(const proc::Block& b, const float* last) {
    ng_ = ng_next_, ng_next_ = 0;
    const bool live = p_[1] / 64 == 0;
    if (!live) { ring_on_ = false; return; }
    if (!ring_on_) { ring_on_ = true; seam(); return; }   // a new source (or the first block): no backlog
    const float hv = v(p_[11]);
    const bool frozen = hv >= (float)kHoldFreeze;
    const float fb = frozen ? 0.0f : 0.95f * c01(hv / (float)(kHoldFreeze - 1));
    if (frozen) { was_frozen_ = true; return; }
    if (was_frozen_) was_frozen_ = false, seam();
    uint32_t w = ring_w_;
    for (int j = 0; j < b.frames; ++j) {
      float x = 0.5f * (in(b, 0, j) + in(b, 1, j));
      if (fb > 0) x = softr(x + fb * (last ? last[j] : 0.0f));
      if (seam_in_) {
        float u = 1.0f - (float)seam_in_ / (float)(kSeam + 1);
        x *= u * u * (3.0f - 2.0f * u);
        --seam_in_;
      }
      ring_[w & (kRing - 1)] = x;
      ++w;
    }
    ring_w_ = w;
  }
  // one grain's samples into acc from s0 on (upstream's play_grain, mono). False: it ended
  template <bool LIVE>
  static bool play(G& g, const float* ring, const int16_t* data, uint32_t len, int ch, const float* eb, float* acc, int nfr) {
    const float* wt = grain_window(g.win);
    double pos = g.pos;
    float wph = g.wph, ofade = g.ofade;
    const bool orph = g.odec > 0;
    int s = g.s0;
    g.s0 = 0;
    for (; s < nfr && wph < 1.0f; ++s) {
      float a, bb, t;
      if (LIVE) {
        int64_t i = (int64_t)pos;
        t = (float)(pos - (double)i);
        a = ring[(uint32_t)i & (kRing - 1)], bb = ring[(uint32_t)(i + 1) & (kRing - 1)];
      } else {
        uint32_t i = (uint32_t)pos;
        t = (float)(pos - (double)i);
        uint32_t i1 = i + 1 >= len ? 0 : i + 1;
        if (ch == 2) {
          a = (float)(int16_t)(((int32_t)data[2 * i] + data[2 * i + 1]) / 2) * (1.0f / 32768.0f);
          bb = (float)(int16_t)(((int32_t)data[2 * i1] + data[2 * i1 + 1]) / 2) * (1.0f / 32768.0f);
        } else {
          a = data[i] * (1.0f / 32768.0f), bb = data[i1] * (1.0f / 32768.0f);
        }
      }
      float x = wph * kGrainWin;
      int wi = (int)x;
      float wv = wt[wi] + (wt[wi + 1] - wt[wi]) * (x - (float)wi);
      float e = (wph < 1.0f - wph ? wph : 1.0f - wph) * g.ek;   // the edge fades (smoothstep)
      if (e < 1.0f) wv *= e * e * (3.0f - 2.0f * e);
      float m;
      if (orph) {
        m = g.oenv * ofade * ofade * (3.0f - 2.0f * ofade);
        ofade -= g.odec;
        if (ofade <= 0) { wph = 1.0f; break; }
      } else {
        m = eb[s];
      }
      acc[s] += (a + (bb - a) * t) * wv * g.gain * m;
      pos += g.inc;
      if (!LIVE) {
        if (pos >= len) pos -= len;
        else if (pos < 0) pos += len;
      }
      wph += g.winc;
    }
    g.pos = pos, g.wph = wph, g.ofade = ofade;
    return wph < 1.0f;
  }
};

}  // namespace dsp
}  // namespace core
