// core/dsp/poly.h - POLY, the DSP library's host of notes (2026-11-10; Connor: library extensions "provided they're not
// overly specific to the mods they're used in"). A slot that plays several notes at once: a chain of up to kChain
// library blocks (any of them: OSC -> FILTER -> ENV, SAMPLE, GRAIN -> ENV, a shipped block) built kNotes times, one
// copy a note, the copies' outputs summed at a headroom gain. Its chord params turn one trig into a chord. Nothing here
// knows a machine: upstream's WAVETABLE is POLY {WTOSC, FILTER, ENV} with its settings, GRANULAR POLY {GRAIN, ENV}.
//
// Placed by dsp's CHAIN op (dsp.h), which names the blocks, the notes (1..kNotes), the weight source and the
// internal mod points; a plain PLACE of "POLY" is refused.
//
// Params (14 bits):
//   0..63    block b's param p at 16 b + p, to every copy (a mod point from outside, core/route, moves it for all)
//   64 CHD   65 INV   66 SPR   67 VOI   the chord (value = the 7-bit index x 64; upstream core/poly.h's table and
//            expansion: CHD 0 OFF 1 MAJ 2 MIN 3 SUS2 4 SUS4 5 7 6 MAJ7 7 MIN7 8 DIM 9 AUG 10 6 11 MIN6 12 9 13 ADD9
//            14 MIN9 15 5; INV 0..3; SPR 0..2; VOI 0 AUTO, 1..6 notes)
//   68..71   internal mod point k's depth (8192 none, 0 = -1, 16383 = +1 x the param's whole range)
//   72 VCRV  each note's velocity gain: 0 none (unity), 1 upstream's 0.25 + 0.75 v^2 (WAVETABLE, GRANULAR)
//   73 NLEV  each note's level, value / 100 (100 unity; upstream WAVETABLE's 50)
//   74 LLEV  a live note's level on top, value / 100 (100 unity; upstream's 60)
// Internal mod points: a copy's block a's control output (ctl c) onto the same copy's block b's param: per note (a
// note's own envelope moving its own oscillator's position), applied before the copy's chain runs, from the ctl as the
// block left it (one block behind the source within the block's chain order, as the route's mod points).
//
// Notes. A note on from the Rytm (midi() src proc::kSrcRytm, core/dsp's TRIG) releases the earlier Rytm notes (their
// tails go on) and starts the chord on its key (one note with CHD OFF); its note off releases them. A USB-A device's
// note on / off (another src) starts / releases one note per key and channel (a live note, left alone by trigs).
// Another message goes to every copy's blocks. A note's copy: a free one, else the oldest released, else the oldest
// (a steal: its blocks' link_reset(), then the note). Release: a note off to the copy's blocks. A copy is free again
// when its weight source's block is done() (proc.h: an ENV at rest), or with none, any of its blocks (a one-shot
// SAMPLE past its end). A note's gain: NLEV, LLEV for a live one, VCRV's velocity law.
// Headroom (upstream's msamp / WAVETABLE / poly FM): the sum times 1 / sqrt(max(1, E)), E the sounding notes' weights
// (the weight source's ctl as the note's block left it, or 1 a note with none), smoothed one pole per block (3 ms
// down, 15 ms up) and ramped over the block. One note plays at unity.
#pragma once
#include <new>

#include "blocks.h"
#include "dsp.h"

namespace core {
namespace dsp {

namespace chord {   // upstream core/poly.h's chords (trigcondition/sidecar-firmware dev core/poly.cpp, as is)
constexpr int kChords = 16, kInvs = 4, kSprs = 3, kVoiMax = 6;
struct Chord { int chd, inv, spr, voi; };
struct Def { uint8_t n; int8_t iv[5]; };
constexpr Def kDefs[kChords] = {
    {1, {0}},           {3, {0, 4, 7}},      {3, {0, 3, 7}},         {3, {0, 2, 7}},
    {3, {0, 5, 7}},     {4, {0, 4, 7, 10}},  {4, {0, 4, 7, 11}},     {4, {0, 3, 7, 10}},
    {3, {0, 3, 6}},     {3, {0, 4, 8}},      {4, {0, 4, 7, 9}},      {4, {0, 3, 7, 9}},
    {5, {0, 4, 7, 10, 14}}, {4, {0, 4, 7, 14}}, {5, {0, 3, 7, 10, 14}}, {2, {0, 7}},
};
inline int idx(int v, int n) { return v >= 0 && v < n ? v : 0; }
inline void sort_unique(int* t, int* m) {
  for (int i = 1; i < *m; ++i)
    for (int j = i; j > 0 && t[j - 1] > t[j]; --j) { int x = t[j]; t[j] = t[j - 1]; t[j - 1] = x; }
  int k = 0;
  for (int i = 0; i < *m; ++i) if (k == 0 || t[i] != t[k - 1]) t[k++] = t[i];
  *m = k;
}
// the chord's notes from 0, ascending and distinct; the count
inline int pattern(const Chord& c, int* t) {
  const Def& d = kDefs[c.chd >= 0 && c.chd < kChords ? c.chd : 0];
  int n = d.n;
  if (c.chd == 0) { t[0] = 0; return 1; }
  int b[5];
  for (int i = 0; i < n; ++i) b[i] = d.iv[i];
  int inv = c.inv % n;
  for (int i = 0; i < inv; ++i) b[i] += 12;
  int nb = n;
  sort_unique(b, &nb);
  int m = c.voi >= 1 && c.voi <= kVoiMax ? c.voi : n;
  for (int k = 0; k < m; ++k) t[k] = b[k % n] + 12 * (k / n);
  sort_unique(t, &m);
  for (int k = 0; k < m; ++k) t[k] += 12 * ((k * (c.spr + 1)) / m);
  sort_unique(t, &m);
  return m;
}
// the chord on root in 0..127 (others dropped); the count
inline int expand(int root, const Chord& c, uint8_t* out) {
  int t[kVoiMax + 2];
  int m = pattern(c, t), k = 0;
  for (int i = 0; i < m; ++i) {
    int x = root + t[i];
    if (x >= 0 && x <= 127) out[k++] = (uint8_t)x;
  }
  return k;
}
}  // namespace chord

class Poly : public proc::Proc {
 public:
  static constexpr int kNotes = 8, kChain = 4, kMods = 4;
  static constexpr int kOwn = 64;                  // the first of POLY's own params
  static constexpr int kParamCount = kOwn + 4 + kMods + 3;
  static constexpr uint8_t kNoWeight = 0x7F;
  struct Spec {
    int notes, nb, nm;
    const BlockInfo* blk[kChain];
    uint8_t weight;                                // b << 4 | ctl, or kNoWeight
    uint8_t mods[kMods][2];                        // {src b << 4 | ctl, dst b << 4 | param}
  };
  static size_t align(size_t n) { return (n + 63) & ~(size_t)63; }
  // the slot memory a spec takes (POLY, then its copies)
  static size_t bytes(const Spec& s) {
    size_t n = align(sizeof(Poly));
    for (int b = 0; b < s.nb; ++b)
      n += align(s.blk[b]->bytes) + (size_t)(s.notes - 1) * align(s.blk[b]->bytes - s.blk[b]->shared);
    return n;
  }
  // built at mem (bytes(s) of it); ok() false: a block refused to build (the copies built so far are gone again)
  Poly(const Spec& s, uint8_t* mem) : s_(s) {
    uint8_t* at = mem + align(sizeof(Poly));
    for (int k = 0; k < s_.notes && ok_; ++k)
      for (int b = 0; b < s_.nb && ok_; ++b) {
        c_[k][b] = s_.blk[b]->make(at, s_.blk[b]->ctx);
        at += align(k ? s_.blk[b]->bytes - s_.blk[b]->shared : s_.blk[b]->bytes);
        if (!c_[k][b]) ok_ = false;
        else if (k) c_[k][b]->share(c_[0][b]);
      }
    for (int m = 0; m < kMods; ++m) depth_[m] = 8192;
    for (int p = 0; p < kOwn; ++p) base_[p] = 0;
    for (int b = 0; b < s_.nb; ++b)   // the bases: the blocks' defaults, as each copy starts
      for (int p = 0; p < s_.blk[b]->nparams && p < 16; ++p) base_[16 * b + p] = s_.blk[b]->params ? s_.blk[b]->params[p].def : 0;
    if (!ok_) destroy();
  }
  ~Poly() override { destroy(); }
  bool ok() const { return ok_; }
  const char* name() const override { return "POLY"; }

  void set_param(int param, int v) override {
    v = v < 0 ? 0 : v > 16383 ? 16383 : v;
    if (param >= 0 && param < kOwn) {
      int b = param >> 4, p = param & 15;
      if (b >= s_.nb) return;
      base_[param] = (uint16_t)v;
      for (int k = 0; k < s_.notes; ++k) c_[k][b]->set_param(p, v);
    } else if (param >= kOwn && param < kOwn + 4) {
      own_[param - kOwn] = v / 64;
    } else if (param >= kOwn + 4 && param < kOwn + 4 + kMods) {
      depth_[param - kOwn - 4] = (uint16_t)v;
    } else if (param >= kOwn + 4 + kMods && param < kParamCount) {
      gain_[param - kOwn - 4 - kMods] = v;
    }
  }
  void link_reset() override {
    for (int k = 0; k < s_.notes; ++k) { reset_copy(k); n_[k] = Note{}; }
    head_ = 1.0f;
  }
  void midi(int src, uint8_t status, uint8_t d1, uint8_t d2) override {
    const uint8_t st = status & 0xF0;
    const bool on = st == 0x90 && d2, off = st == 0x80 || (st == 0x90 && !d2);
    if (src == proc::kSrcRytm && on) {
      for (int k = 0; k < s_.notes; ++k) if (!n_[k].live) release(k);
      chord::Chord c{chord::idx(own_[0], chord::kChords), chord::idx(own_[1], chord::kInvs),
                     chord::idx(own_[2], chord::kSprs), chord::idx(own_[3], chord::kVoiMax + 1)};
      uint8_t ks[chord::kVoiMax];
      int n = chord::expand(d1, c, ks);
      for (int i = 0; i < n; ++i) start(ks[i], d2, false, 0);
    } else if (src == proc::kSrcRytm && off) {
      for (int k = 0; k < s_.notes; ++k) if (!n_[k].live) release(k);
    } else if (on) {
      start(d1, d2, true, (uint8_t)(src << 4 | (status & 15)));
    } else if (off) {
      for (int k = 0; k < s_.notes; ++k)
        if (n_[k].live && n_[k].key == d1 && n_[k].tag == (uint8_t)(src << 4 | (status & 15))) release(k);
    } else {
      for (int k = 0; k < s_.notes; ++k)
        for (int b = 0; b < s_.nb; ++b) c_[k][b]->midi(src, status, d1, d2);
    }
  }

  void process(const proc::Block& b) override {
    const int nf = b.frames;
    float* acc[2] = {acc_[0], acc_[1]};
    memset(acc_, 0, sizeof acc_);
    for (int i = 0; i < s_.nb; ++i) c_[0][i]->voice(b, last_);   // the voice-wide work, before the notes
    float e = 0.0f;
    bool any = false;
    for (int k = 0; k < s_.notes; ++k) {
      Note& n = n_[k];
      if (!n.on) continue;
      apply_mods(k);
      const float* in[2] = {b.sin[0], b.sin[1]};
      for (int i = 0; i < s_.nb; ++i) {
        proc::Block cb = b;
        float* o = buf_[i & 1][0];
        cb.sin[0] = in[0]; cb.sin[1] = in[1];
        cb.sout[0] = buf_[i & 1][0]; cb.sout[1] = buf_[i & 1][1];
        memset(buf_[i & 1], 0, sizeof buf_[0]);
        c_[k][i]->process(cb);
        in[0] = o; in[1] = buf_[i & 1][1];
      }
      for (int f = 0; f < nf; ++f) { acc[0][f] += in[0][f] * n.gain; acc[1][f] += in[1][f] * n.gain; }
      any = true;
      bool fin = false;   // the weight's block at rest (a mod envelope's end leaves the note on), else any block
      if (s_.weight != kNoWeight) fin = c_[k][s_.weight >> 4 & 3]->done();
      else for (int i = 0; i < s_.nb; ++i) fin = fin || c_[k][i]->done();
      if (fin) { n = Note{}; continue; }
      e += s_.weight == kNoWeight ? 1.0f : c_[k][s_.weight >> 4 & 3]->ctl(s_.weight & 15);
    }
    // the headroom (upstream: 1 / sqrt of the notes' weights, one pole per block, ramped over it)
    const float atk = 1.0f - expf(-1000.0f * (float)nf / (kRate * 3.0f));
    const float rel = 1.0f - expf(-1000.0f * (float)nf / (kRate * 15.0f));
    float g0 = head_, target = e > 1.0f ? 1.0f / sqrtf(e) : 1.0f;
    float g1 = g0 + (target - g0) * (target < g0 ? atk : rel);
    if (fabsf(g1 - target) < 1e-5f) g1 = target;
    head_ = g1;
    const float dg = (g1 - g0) / (float)nf;
    for (int f = 0; f < nf; ++f) {
      float g = any ? g0 + dg * (float)(f + 1) : 0.0f;
      if (b.sout[0]) b.sout[0][f] = acc[0][f] * g;
      if (b.sout[1]) b.sout[1][f] = acc[1][f] * g;
      last_[f] = acc[0][f] * g;
    }
    for (int f = nf; f < proc::kMaxFrames; ++f) last_[f] = 0.0f;
  }
  bool done() const override {
    for (int k = 0; k < s_.notes; ++k) if (n_[k].on) return false;
    return true;
  }

  // tests
  int sounding() const { int a = 0; for (int k = 0; k < s_.notes; ++k) a += n_[k].on; return a; }
  int held() const { int a = 0; for (int k = 0; k < s_.notes; ++k) a += n_[k].on && n_[k].held; return a; }
  float headroom() const { return head_; }
  proc::Proc* copy(int k, int b) const { return c_[k][b]; }
  int key(int k) const { return n_[k].on ? n_[k].key : -1; }

 private:
  struct Note { bool on, held, live; uint8_t key, tag; uint32_t order; float gain; };
  Spec s_;
  proc::Proc* c_[kNotes][kChain] = {};
  Note n_[kNotes] = {};
  uint16_t base_[kOwn];
  uint16_t depth_[kMods];
  int own_[4] = {};
  int gain_[3] = {0, 100 * 64, 100 * 64};   // VCRV NLEV LLEV
  uint32_t order_ = 0;
  float head_ = 1.0f;
  bool ok_ = true;
  float buf_[2][2][proc::kMaxFrames];
  float acc_[2][proc::kMaxFrames];
  float last_[proc::kMaxFrames] = {};   // its output's L, the last block (a block's feedback: GRAIN's ring)

  void destroy() {
    for (int k = 0; k < kNotes; ++k)
      for (int b = 0; b < kChain; ++b)
        if (c_[k][b]) { c_[k][b]->~Proc(); c_[k][b] = nullptr; }
  }
  void reset_copy(int k) { for (int b = 0; b < s_.nb; ++b) c_[k][b]->link_reset(); }
  void to_copy(int k, uint8_t st, uint8_t d1, uint8_t d2) {
    for (int b = 0; b < s_.nb; ++b) c_[k][b]->midi(proc::kSrcRytm, st, d1, d2);
  }
  int pick() const {
    int b = -1;
    for (int k = 0; k < s_.notes; ++k) if (!n_[k].on) return k;
    for (int k = 0; k < s_.notes; ++k)   // the oldest released
      if (!n_[k].held && (b < 0 || (int32_t)(n_[k].order - n_[b].order) < 0)) b = k;
    if (b < 0)
      for (int k = 0; k < s_.notes; ++k)
        if (b < 0 || (int32_t)(n_[k].order - n_[b].order) < 0) b = k;
    return b;
  }
  void start(uint8_t key, uint8_t vel, bool live, uint8_t tag) {
    int k = pick();
    if (n_[k].on) reset_copy(k);   // a steal
    float v = (vel < 1 ? 1 : vel) / 127.0f, g = gain_[1] / 64 / 100.0f;
    if (gain_[0] / 64 >= 1) g *= 0.25f + 0.75f * v * v;
    if (live) g *= gain_[2] / 64 / 100.0f;
    n_[k] = Note{true, true, live, key, tag, ++order_, g};
    to_copy(k, 0x90, key, vel ? vel : 1);
  }
  void release(int k) {
    if (!n_[k].on || !n_[k].held) return;
    n_[k].held = false;
    to_copy(k, 0x80, n_[k].key, 0);
  }
  void apply_mods(int k) {   // the copy's modulated params: base + the sum of their points' ctl x depth
    float off[kMods];
    int dst[kMods], nd = 0;
    for (int m = 0; m < s_.nm; ++m) {
      const int sb = s_.mods[m][0] >> 4, sc = s_.mods[m][0] & 15, d = s_.mods[m][1];
      float o = c_[k][sb]->ctl(sc) * ((float)depth_[m] - 8192.0f) / 8192.0f * 16383.0f;
      int j = 0;
      while (j < nd && dst[j] != d) ++j;
      if (j == nd) { dst[nd] = d; off[nd++] = 0.0f; }
      off[j] += o;
    }
    for (int j = 0; j < nd; ++j) {
      int v = base_[(dst[j] >> 4) * 16 + (dst[j] & 15)] + (int)(off[j] + (off[j] < 0 ? -0.5f : 0.5f));
      c_[k][dst[j] >> 4]->set_param(dst[j] & 15, v < 0 ? 0 : v > 16383 ? 16383 : v);
    }
  }
};

}  // namespace dsp
}  // namespace core
