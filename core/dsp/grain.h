// core/dsp/grain.h - GRAIN, the DSP library's granular block (2026-11-10, ext's GRANULAR on the library; the params
// follow upstream's GRANULAR machine, core/granular/granular.h on trigcondition/sidecar-firmware dev, as a block with
// no envelope, chords or animation of its own: ENV, mod points and MOD's LFO do those in a graph).
//
// A cloud of short windowed grains read from a source at a position. The source: a pool entry (core/samples; SMP n
// is entry n - 1, a sample) or, SMP 0, the block's own input (L + R) recorded into a ring of kRing frames (5.5 s)
// while not frozen. A note sets the pitch (60 = the source's own) and, from a pool entry, starts the playhead at POS;
// from a pool entry the cloud sounds from the first note on, from the input always.
//
// Params (14 bits; the Rytm's knob words >> 2, so value x 64 + the fraction's top 6 bits):
//   P0 LEV   level, value / 100 (unity at 100)
//   P1 SMP   0 the input; n a pool entry n - 1
//   P2 POS   a pool entry: where the playhead starts (0 the start .. 127 the end); the input: how far back the grains
//            read (0 the newest .. 127 5 s ago)
//   P3 SPRY  each grain's position jittered back by up to 2 s (squared), its start time by up to +-50 %
//   P4 SIZE  5 ms .. 1 s (exponential)
//   P5 DENS  1 .. 200 grains a second (exponential)
//   P6 PRND  per-grain random pitch, up to +-12 semitones
//   P7 REV   the probability a grain plays reversed (0 .. 100 %)
//   P8 WIN   zones: HANN TRI TUKEY PERC
//   P9 HOLD  0..119: a pool entry - the playhead's speed, 1x at 0 (time-stretched at the note's pitch) slowing to 0 at
//            119; the input - the feedback, the output into its ring (0 .. 0.95, soft-clipped); 120..127 FREEZE (the
//            playhead stops / the ring stops recording)
//   P10 TUN  semitones from 64        P11 FIN  cents from 64 x 64
// Each grain at 1 / sqrt(the overlap, DENS x SIZE), so the cloud's level stays even. Mono on L and R.
#pragma once
#include "../samples/samples.h"
#include "blocks.h"

namespace core {
namespace dsp {

class Grain : public Block {
 public:
  static constexpr uint32_t kRing = 1u << 18;   // 5.46 s
  static constexpr int kGrains = 64;
  static constexpr int kWin = 1024;
  Grain() {
    p_[0] = 100 * 64; p_[1] = 0; p_[2] = 0; p_[3] = 0; p_[4] = 64 * 64; p_[5] = 64 * 64; p_[6] = 0; p_[7] = 0;
    p_[8] = 0; p_[9] = 0; p_[10] = 64 * 64; p_[11] = 64 * 64;
    memset(ring_, 0, sizeof ring_);
    for (int w = 0; w < 4; ++w)
      for (int i = 0; i <= kWin; ++i) win_[w][i] = window(w, (float)i / kWin);
  }
  const char* name() const override { return "GRAIN"; }
  void link_reset() override { memset(ring_, 0, sizeof ring_); for (G& g : g_) g.on = false; }
  void midi(int, uint8_t status, uint8_t d1, uint8_t d2) override {
    if ((status & 0xF0) != 0x90 || !d2) return;
    note_ = d1;
    noted_ = true;
  }
  void set_param(int param, int value14) override {
    Block::set_param(param, value14);
    if (param == 2) repos_ = true;   // a new POS: the playhead goes there (once a note started it)
  }
  void process(const proc::Block& b) override {
    samples::View v{};
    const int smp = p_[1] / 64;
    const bool card = smp > 0;
    const bool have = card && samples::get(smp - 1, &v) && v.frames > 1;
    const float lev = val(p_[0]) / 100.0f, pos = val(p_[2]) / 127.0f, spry = val(p_[3]) / 127.0f;
    const float size = 0.005f * powf(200.0f, val(p_[4]) / 127.0f), dens = powf(200.0f, val(p_[5]) / 127.0f);
    const float prnd = val(p_[6]) / 127.0f * 12.0f, rev = val(p_[7]) / 127.0f;
    const int win = zone(p_[8], 4);
    const float hold = val(p_[9]);
    const bool freeze = hold >= 120.0f;
    const float semis = (float)note_ - 60.0f + (val(p_[10]) - 64.0f) + (val(p_[11]) - 64.0f) / 64.0f;
    const float pitch = exp2f(semis / 12.0f);
    const float fb = card || freeze ? 0.0f : hold / 119.0f * 0.95f;
    const float speed = card && !freeze ? 1.0f - hold / 119.0f : 0.0f;
    const float g_each = 1.0f / sqrtf(fmaxf(1.0f, dens * size));
    const float len = size * kRate, interval = kRate / dens;
    const float rate = have ? v.rate / kRate : 1.0f;
    const float src_len = have ? (float)(v.frames - 1) : (float)kRing;
    if (card != card_) { card_ = card; for (G& g : g_) g.on = false; started_ = false; }
    if (card && (noted_ || (repos_ && started_))) { head_ = pos * src_len; started_ = true; }
    noted_ = repos_ = false;
    const bool run = card ? have && started_ : true;
    for (int f = 0; f < b.frames; ++f) {
      float x = 0.5f * (in(b, 0, f) + in(b, 1, f));
      // a new grain when it is due
      if (run && --next_ <= 0.0f) {
        next_ += interval * (1.0f + spry * 0.5f * (2.0f * rnd() - 1.0f));
        if (next_ < 1.0f) next_ = 1.0f;
        spawn(card, src_len, pos, spry, rate, pitch, prnd, rev, len, g_each);
      }
      float y = 0.0f;
      for (G& g : g_) {
        if (!g.on) continue;
        float t = g.age / g.len;
        float wi = t * kWin;
        int i = (int)wi;
        float w = win_[win][i] + (win_[win][i + 1] - win_[win][i]) * (wi - (float)i);
        y += w * g.gain * (card ? read_card(v, g.pos) : read_ring(g.pos));
        g.pos += g.step;
        if (!card) g.pos = g.pos >= (float)kRing ? g.pos - (float)kRing : g.pos < 0.0f ? g.pos + (float)kRing : g.pos;
        if (++g.age >= g.len) g.on = false;
      }
      y *= lev;
      if (!card) {   // the ring: the input (and the feedback) unless frozen
        if (!freeze) {
          float r = x + fb * soft(y);
          ring_[w_ & (kRing - 1)] = (int16_t)(r > 1.0f ? 32767 : r < -1.0f ? -32767 : r * 32767.0f);
          w_ = (w_ + 1) & (kRing - 1);
        }
      } else if (run) {
        head_ += speed * rate;
        if (head_ >= src_len) head_ = pos * src_len;   // round again from POS
      }
      out(b, 0, f, y);
      out(b, 1, f, y);
    }
  }

 private:
  struct G { bool on; float pos, step, age, len, gain; };
  G g_[kGrains] = {};
  int16_t ring_[kRing];
  float win_[4][kWin + 1];
  uint32_t w_ = 0, seed_ = 0x1234567u;
  float head_ = 0.0f, next_ = 0.0f;
  int note_ = 60;
  bool noted_ = false, repos_ = false, started_ = false, card_ = false;

  static float val(int v) { return (float)v / 64.0f; }
  float rnd() { seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5; return (float)(seed_ >> 8) * (1.0f / 16777216.0f); }
  static float window(int w, float t) {
    switch (w) {
      case 0: return 0.5f - 0.5f * cosf(2.0f * kPi * t);                        // HANN
      case 1: return 1.0f - fabsf(2.0f * t - 1.0f);                             // TRI
      case 2: return t < 0.2f ? 0.5f - 0.5f * cosf(kPi * t / 0.2f)              // TUKEY (20 % tapers)
                    : t > 0.8f ? 0.5f - 0.5f * cosf(kPi * (1.0f - t) / 0.2f) : 1.0f;
      default: return t < 0.02f ? t / 0.02f : powf(1.0f - (t - 0.02f) / 0.98f, 2.0f);   // PERC
    }
  }
  void spawn(bool card, float src_len, float pos, float spry, float rate, float pitch, float prnd, float rev, float len,
             float gain) {
    G* g = nullptr;
    for (G& c : g_) if (!c.on) { g = &c; break; }
    if (!g) return;   // the cloud is full: left out
    float back = spry * spry * 2.0f * kRate * rnd();
    float p;
    float st = pitch * exp2f(prnd * (2.0f * rnd() - 1.0f) / 12.0f) * rate;
    bool r = rnd() < rev;
    if (card) {
      p = head_ - back * rate;
      if (p < 0.0f) p = 0.0f;
    } else {   // frames behind the newest, in the ring's own index (a forward grain never reads past the newest)
      float d = 2.0f + pos * 5.0f * kRate + back;
      if (!r && d < st * len + 2.0f) d = st * len + 2.0f;
      if (d > (float)(kRing - 2)) d = (float)(kRing - 2);
      p = (float)(w_ & (kRing - 1)) - d;
      if (p < 0.0f) p += (float)kRing;
    }
    *g = G{true, p, r ? -st : st, 0.0f, len < 2.0f ? 2.0f : len, gain};
    if (card && r) g->pos = p + st * len < src_len ? p + st * len : src_len;   // reversed: from its end back
    (void)src_len;
  }
  static float read_card(const samples::View& v, float pos) {
    if (pos < 0.0f || pos >= (float)(v.frames - 1)) return 0.0f;
    uint32_t i = (uint32_t)pos;
    float fr = pos - (float)i;
    const int16_t* a = v.pcm + (size_t)i * v.ch;
    const int16_t* c = a + v.ch;
    float x = a[0] + fr * (c[0] - a[0]);
    if (v.ch > 1) x = 0.5f * (x + a[1] + fr * (c[1] - a[1]));
    return x * (1.0f / 32768.0f);
  }
  float read_ring(float pos) const {
    float fp = floorf(pos);
    int64_t i = (int64_t)fp;
    float fr = pos - fp;
    float a = ring_[(uint32_t)i & (kRing - 1)], c = ring_[(uint32_t)(i + 1) & (kRing - 1)];
    return (a + fr * (c - a)) * (1.0f / 32768.0f);
  }
};

}  // namespace dsp
}  // namespace core
