// core/dsp/sample.h - SAMPLE, the DSP library's sample player (2026-11-10, with core/samples): plays one entry of the
// project's sample pool on each note its slot gets (a Rytm voice machine's trig through core/dsp's TRIG, or a USB-A
// device's note), in RAM, at the note's pitch. Generic: any mod may place it; DSP SAMPLE is the first.
//
// Params (14 bits; the Rytm's knob words >> 2, so value x 64 + the fraction's top 6 bits):
//   P0 LEV   output level, value / 100 (unity at 100)
//   P1 SMP   the pool entry: value 0 is OFF, value n is entry n - 1 (core/samples' index)
//   P2 TUN   semitones from 64 (fractions included), added to the note's distance from 60 (C4 plays at the file's rate)
//   P3 STA   where it starts, 0..127 over the sample's length
//   P4 END   where it ends, 0..127 over the length (127.98: the last frame); END before STA plays backwards
//   P5 LOOP  0 one shot; 1 and up (value >= 1): loop STA..END
//   P6 FIN   cents from 64 x 64 (+-100 over the range)
//   P7 VEL   how much the note's velocity sets the level: 0 none, 127 all (velocity 127 is unity either way)
//   P8 SLN   slices (2026-11-10, CHOP on the library): 0 off; n (1..64) cuts STA..END into n equal slices, and a note
//            plays slice SLC + (note - 60), modulo n, at TUN / FIN only (the note picks the slice, not the pitch)
//   P9 SLC   the slice a note 60 (a plain trig) plays, 0..63 (p-lockable: a slice a step)
//   P10 RTRG an instrument's release regions: 0..63 off, 64.. a note off starts them (upstream MULTISAMPLE's RTRG)
//   P11 ATK  P12 DEC  P13 SUS  P14 REL   an instrument's ampeg_*: 64 as the file says, times x 2^((value - 64) / 16),
//            SUS below 64 scaled down, above toward 100 % (upstream MULTISAMPLE's)
//   P15 OUT  an instrument's output: 0 mono on L and R (upstream render_add: a stereo sample's channels summed),
//            1 stereo (upstream render_add2: the region's pan / width / position)
// An instrument entry (core/samples: an .sfz / .elmulti; MULTISAMPLE, CHOP's slice maps) plays as upstream MULTISAMPLE's
// note (core/msamp/msamp.cpp start_region / render): a note starts every region its key, velocity, round robin and
// random pick (up to kLayers), each at its own pitch (key centre, keytrack, transpose, tune, TUN, FIN), level (volume,
// amplitude, velocity tracking x VEL, LEV), start (offset + STA's x / 128 of offset..end), loop (the region's, else
// the file's smpl loop; LOOP's zones AUTO / OFF / ON), AHDSR envelope and Hermite read. A new note on releases the
// layers that play (their tails go on; a one-shot is never released); a note off releases them once they have sounded
// and, with RTRG, starts the key's release regions; a host's cut (Proc::cut, a new trig in a POLY) releases them only.
// ctl(0) is the loudest layer's envelope (POLY's weight, with HKEY one a key); done() once every layer ended. END,
// SLN, SLC are not used on one.
// A sample (SMP not an instrument) starts at STA, with a 1 ms fade-in; a new note while it plays starts over (the old one
// is cut, faded over 1 ms). A note off does nothing (the Rytm's amp and the mod decide the length). Output: the
// sample's L and R (a mono file on both), linear interpolation, read where core/samples published it (lock-free; an
// entry that is not READY is silence).
#pragma once
#include "../samples/samples.h"
#include "blocks.h"

namespace core {
namespace dsp {

class Sample : public Block {
 public:
  Sample() {
    p_[0] = 100 * 64; p_[1] = 0; p_[2] = 64 * 64; p_[3] = 0; p_[4] = 16383; p_[5] = 0; p_[6] = 64 * 64; p_[7] = 0;
    p_[8] = 0; p_[9] = 0; p_[10] = 0; p_[11] = 64 * 64; p_[12] = 64 * 64; p_[13] = 64 * 64; p_[14] = 64 * 64;
    p_[15] = 0;
  }
  const char* name() const override { return "SAMPLE"; }
  void link_reset() override {
    play_ = false, pending_ = false;
    for (Layer& l : ly_) l.on = false;
  }
  bool done() const override {
    if (pending_ || play_ || fade_out_ > 0) return false;
    for (const Layer& l : ly_) if (l.on) return false;
    return true;
  }
  float ctl(int k) const override { return k == 0 ? weight_ : 0.0f; }
  void midi(int src, uint8_t status, uint8_t d1, uint8_t d2) override {
    const uint8_t st = status & 0xF0;
    if (st == 0x90 && d2) {
      note_ = d1;
      vel_ = d2;
      pending_ = true, pend_off_ = false;
      if (samples::instrument(p_[1] / 64 - 1)) release(false);   // the earlier note's layers: their tails go on
    } else if (st == 0x80 || st == 0x90) {
      if (pending_) pend_off_ = true;   // its note on not started yet: released after it (once it has sounded)
      else release(true);
    }
  }
  void cut(uint8_t) override { release(false); }
  void process(const proc::Block& b) override {
    samples::View v, ov;
    const int smp = p_[1] / 64;
    if (smp > 0 && samples::instrument(smp - 1)) {
      play_ = false, fade_out_ = 0;
      if (pending_) {
        pending_ = false;
        key_vel_ = vel_;
        start_layers(smp - 1, note_, vel_, 0);
        if (pend_off_) release(true);
      }
      pend_off_ = false;
      if (rel_regions_) {
        rel_regions_ = false;
        start_layers(smp - 1, note_, key_vel_, 1);
      }
      render_layers(b);
      return;
    }
    for (Layer& l : ly_) l.on = false;
    weight_ = 0.0f;
    if (pending_) {   // a note: SMP's sample
      pending_ = false;
      const int src = smp - 1;
      if (smp > 0 && samples::get(src, &v) && v.frames > 1) {
        if (play_) { fade_out_ = kFade; old_src_ = src_; }
        play_ = true;
        entry_ = smp;
        src_ = src;
        start(v);
        fade_in_ = 0;
      }
    }
    const bool have = smp > 0 && smp == entry_ && samples::get(src_, &v) && v.frames > 1;
    if (!have) play_ = false;   // OFF, another entry or gone: silence until the next note
    const bool ohave = fade_out_ > 0 && samples::get(old_src_, &ov) && ov.frames > 1;
    const float vamt = (float)p_[7] / 16383.0f, vel = (float)vel_ / 127.0f;
    float gain = (float)p_[0] / (64.0f * 100.0f) * (1.0f - vamt * (1.0f - vel));
    float semis = ((float)p_[2] / 64.0f - 64.0f) + ((float)p_[6] / 64.0f - 64.0f) / 64.0f;
    if (!slices()) {
      semis += (float)note_ - 60.0f;
    }
    const float step = have ? exp2f(semis / 12.0f) * v.rate / kRate : 0.0f;
    const float ostep = ohave ? exp2f(semis / 12.0f) * ov.rate / kRate : 0.0f;
    for (int f = 0; f < b.frames; ++f) {
      float l = 0.0f, r = 0.0f;
      if (play_) {
        read(v, pos_, &l, &r);
        float g = fade_in_ < kFade ? (float)++fade_in_ / kFade : 1.0f;
        l *= g;
        r *= g;
        pos_ += dir_ * step;
        if (dir_ > 0 && loop_ && pos_ >= lhi_ && lhi_ - llo_ >= 2.0f) pos_ = llo_ + (pos_ - lhi_);
        else if (dir_ < 0 && loop_ && pos_ <= llo_ && lhi_ - llo_ >= 2.0f) pos_ = lhi_ - (llo_ - pos_);
        else if ((dir_ > 0 && pos_ >= hi_) || (dir_ < 0 && pos_ <= lo_)) play_ = false;
      }
      if (fade_out_ > 0) {   // the cut note's last frames, faded
        float ol = 0.0f, orr = 0.0f;
        if (ohave && old_pos_ >= 0.0f && old_pos_ < (float)(ov.frames - 1)) read(ov, old_pos_, &ol, &orr);
        float g = (float)fade_out_-- / kFade;
        l += ol * g;
        r += orr * g;
        old_pos_ += old_dir_ * ostep;
      }
      out(b, 0, f, l * gain);
      out(b, 1, f, r * gain);
    }
  }

  static constexpr int kLayers = 4;   // regions a key starts at most (upstream kLayers)
  int layers_on() const { int n = 0; for (const Layer& l : ly_) n += l.on; return n; }   // tests

 private:
  static constexpr int kFade = 48;   // 1 ms
  bool pending_ = false, play_ = false, loop_ = false;
  int note_ = 60, vel_ = 127, entry_ = 0, src_ = -1, old_src_ = -1, fade_in_ = 0, fade_out_ = 0;
  float pos_ = 0.0f, lo_ = 0.0f, hi_ = 0.0f, llo_ = 0.0f, lhi_ = 0.0f, dir_ = 1.0f, old_pos_ = -1.0f, old_dir_ = 1.0f;

  // an instrument's layers (upstream msamp's Note: its Start and the render state); a release region takes a layer too
  struct Layer {
    bool on, held, relq, rel, sounded, one_shot;
    uint8_t entry, mode, stage;      // mode: 0 none, 1 continuous, 2 sustain (while held)
    uint32_t end, loop_s, loop_e;    // exclusive ends
    double pos;
    float inc, gain, pl, pr, wd;
    uint32_t att, hold, n, order;
    float dcoef, sus, rcoef, lvl;
  };
  static constexpr int kLayerSlots = 2 * kLayers;
  Layer ly_[kLayerSlots] = {};
  bool rel_regions_ = false, pend_off_ = false;
  uint8_t key_vel_ = 127;
  uint32_t order_ = 0, rnd_ = 0x9e3779b9u;
  float weight_ = 0.0f;

  static float vv(int w) { return (float)w / 64.0f; }   // a param's value (upstream's p[])
  static float tscale(float v) { return exp2f((v - 64.0f) / 16.0f); }
  static float coef(float secs) {   // per sample, -60 dB after secs
    float n = secs * 48000.0f;
    return n < 48 ? expf(-6.9078f / 48.0f) : expf(-6.9078f / n);
  }
  // held layers released (a note off: once they have sounded; with RTRG its release regions after)
  void release(bool note_off) {
    bool any = false;
    for (Layer& l : ly_) {
      if (!l.on || !l.held) continue;
      l.held = false, l.relq = true, any = true;
    }
    if (note_off && any && vv(p_[10]) >= 64.0f) rel_regions_ = true;
  }
  int pick_layer() {   // a free layer, else the oldest released, else the oldest
    int b = -1;
    for (int k = 0; k < kLayerSlots; ++k) if (!ly_[k].on) return k;
    for (int k = 0; k < kLayerSlots; ++k)
      if (!ly_[k].held && (b < 0 || (int32_t)(ly_[k].order - ly_[b].order) < 0)) b = k;
    if (b < 0)
      for (int k = 0; k < kLayerSlots; ++k) if (b < 0 || (int32_t)(ly_[k].order - ly_[b].order) < 0) b = k;
    return b;
  }
  // upstream play_key + start_region (RAM: the whole sample is the head)
  void start_layers(int inst, int key, int vel, int trig) {
    rnd_ = rnd_ * 1664525u + 1013904223u;
    const float r01 = (float)(rnd_ >> 8) / 16777216.0f;
    samples::Layer rs[kLayers];
    const int n = samples::layers(inst, key, vel, trig, r01, rs, kLayers);
    for (int i = 0; i < n; ++i) {
      const samples::Layer& r = rs[i];
      samples::View m;
      if (!samples::get(r.entry, &m) || !m.frames) continue;
      Layer& x = ly_[pick_layer()];
      x = Layer{};
      uint32_t e = r.end == samples::kNone || r.end + 1 > m.frames ? m.frames : r.end + 1;
      uint32_t off = r.offset < e ? r.offset : e - 1;
      float strt = vv(p_[3]);
      strt = strt < 0 ? 0 : strt > 127 ? 127 : strt;
      x.pos = off + (double)(e - off) * (strt / 128.0);
      x.entry = r.entry;
      {
        float pn = r.pan / 100.0f;
        if (m.ch == 2) pn += r.position / 100.0f;
        pn = pn < -1 ? -1 : pn > 1 ? 1 : pn;
        if (m.ch == 2) {
          x.pl = pn > 0 ? 1.0f - pn : 1.0f, x.pr = pn < 0 ? 1.0f + pn : 1.0f;
        } else {
          float th = (pn + 1.0f) * 0.78539816f;
          x.pl = 1.41421356f * cosf(th), x.pr = 1.41421356f * sinf(th);
          if (r.pan == 0) x.pl = x.pr = 1.0f;
        }
        x.wd = r.width / 100.0f;
      }
      x.end = e;
      const bool has_loop = m.has_loop ? true : r.iloop && r.iloop_e < m.frames;
      const uint32_t mls = m.has_loop ? m.loop_s : r.iloop_s, mle = m.has_loop ? m.loop_e : r.iloop_e;
      uint32_t ls = r.loop_start != samples::kNone ? r.loop_start : has_loop ? mls : off;
      uint32_t le = r.loop_end != samples::kNone ? r.loop_end + 1 : has_loop ? mle + 1 : e;
      if (le > e) le = e;
      const bool can_loop = le > ls + 1;
      const int lz = vzone(p_[5], 3);   // AUTO OFF ON
      uint8_t mode = r.loop_mode;
      if (mode == samples::kLoopAuto)
        mode = (has_loop || r.loop_start != samples::kNone || r.loop_end != samples::kNone) ? samples::kLoopContinuous
                                                                                              : samples::kNoLoop;
      x.one_shot = mode == samples::kOneShot;
      if (lz == 1) mode = samples::kNoLoop;
      else if (lz == 2 && mode != samples::kLoopSustain) mode = samples::kLoopContinuous;
      x.mode = !can_loop ? 0 : mode == samples::kLoopContinuous ? 1 : mode == samples::kLoopSustain ? 2 : 0;
      x.loop_s = ls, x.loop_e = le;
      float semis = (key - r.keycenter) * r.pitch_keytrack / 100.0f + r.transpose + r.tune / 100.0f +
                    (vv(p_[2]) - 64) + (vv(p_[6]) - 64) / 64.0f;
      x.inc = m.rate / 48000.0f * exp2f(semis / 12.0f);
      const float pv = vv(p_[7]);
      float vt = r.amp_veltrack / 100.0f * (pv < 0 ? 0 : pv > 127 ? 1 : pv / 127.0f);
      if (vt < 0) vt = 0;
      float vn = (vel < 1 ? 1 : vel) / 127.0f;
      float vg = 1 - vt + vt * vn * vn;
      const float pl = vv(p_[0]);
      x.gain = powf(10.0f, r.volume / 20.0f) * r.amplitude / 100.0f * vg * (pl < 0 ? 0 : pl / 100.0f) * 1.0f;
      float att = r.attack * tscale(vv(p_[11])), hold = r.hold, dec = r.decay * tscale(vv(p_[12])),
            rel = r.release * tscale(vv(p_[14]));
      float sus = r.sustain / 100.0f, sv = vv(p_[13]);
      sus = sv <= 64 ? sus * sv / 64.0f : sus + (1 - sus) * (sv - 64) / 63.0f;
      x.att = (uint32_t)(att > 30 ? 30 * 48000 : att * 48000);
      x.hold = (uint32_t)(hold > 30 ? 30 * 48000 : hold * 48000);
      x.dcoef = dec <= 0 ? 0 : coef(dec);
      x.sus = sus < 0 ? 0 : sus > 1 ? 1 : sus;
      x.rcoef = coef(rel);
      x.on = true;
      x.held = !x.one_shot && !trig;
      x.order = ++order_;
    }
  }
  static float herm4(float xm1, float x0, float x1, float x2, float t) {
    float c = (x1 - xm1) * 0.5f, w = x0 - x1, a = c + w + w + (x2 - x0) * 0.5f, bb = c + w + a;
    return ((a * t - bb) * t + c) * t + x0;
  }
  // upstream render_impl's note loop, in RAM (no stream: nothing misses)
  void render_layers(const proc::Block& b) {
    const int nfr = b.frames;
    const bool stereo = vzone(p_[15], 2) == 1;
    float acc[proc::kMaxFrames], accr[proc::kMaxFrames];
    for (int s = 0; s < nfr; ++s) acc[s] = accr[s] = 0.0f;
    float w = 0.0f;
    for (Layer& n : ly_) {
      if (!n.on) continue;
      samples::View m;
      if (!samples::get(n.entry, &m) || !m.frames) { n.on = false; continue; }
      if (!n.rel && !n.one_shot && n.sounded && n.relq) n.rel = true, n.stage = 4;
      const int ch = m.ch == 2 ? 2 : 1;
      const bool two = stereo && ch == 2;
      const float wa = 0.5f * (1.0f + n.wd), wb = 0.5f * (1.0f - n.wd);
      const int64_t end = n.end;
      bool fin = false;
      for (int s = 0; s < nfr; ++s) {
        const bool looping = n.mode == 1 || (n.mode == 2 && !n.rel);
        const int64_t i = (int64_t)n.pos;
        const float t = (float)(n.pos - (double)i);
        const int64_t len = (int64_t)n.loop_e - n.loop_s;
        float x[4], xr[4] = {0, 0, 0, 0};
        for (int j = 0; j < 4; ++j) {
          int64_t f = i - 1 + j;
          if (looping && f >= n.loop_e) f -= len;
          f = f < 0 ? 0 : f >= end ? end - 1 : f;
          const int16_t* q = m.pcm + (size_t)f * ch;
          if (ch == 2) {
            if (two) x[j] = q[0], xr[j] = q[1];
            else x[j] = (float)(((int32_t)q[0] + (int32_t)q[1]) >> 1);
          } else {
            x[j] = q[0];
          }
        }
        switch (n.stage) {   // the envelope
          case 0:
            if (n.n >= n.att) { n.stage = 1, n.n = 0, n.lvl = 1; }
            else { n.lvl = (float)(n.n + 1) / (float)n.att; ++n.n; }
            break;
          case 1:
            if (n.n >= n.hold) n.stage = n.dcoef > 0 ? 2 : 3, n.lvl = n.dcoef > 0 ? 1 : n.sus;
            else ++n.n;
            break;
          case 2:
            n.lvl = n.sus + (n.lvl - n.sus) * n.dcoef;
            if (n.lvl - n.sus < 1e-4f) n.stage = 3, n.lvl = n.sus;
            break;
          case 3: n.lvl = n.sus; break;
          default:
            n.lvl *= n.rcoef;
            if (n.lvl < 1e-4f) fin = true;
            break;
        }
        if (fin) break;
        n.sounded = true;
        const float y = herm4(x[0], x[1], x[2], x[3], t);
        if (stereo) {
          const float g = (1.0f / 32768.0f) * n.lvl * n.gain;
          const float yr = two ? herm4(xr[0], xr[1], xr[2], xr[3], t) : y;
          const float a = two ? wa * y + wb * yr : y, c = two ? wb * y + wa * yr : y;
          acc[s] += a * g * n.pl;
          accr[s] += c * g * n.pr;
        } else {
          acc[s] += y * (1.0f / 32768.0f) * n.lvl * n.gain;
        }
        n.pos += n.inc;
        if (looping && n.pos >= n.loop_e) n.pos -= (double)len;
        else if (n.pos >= (double)end) { fin = true; break; }
      }
      if (fin) { n.on = false; continue; }
      if (n.lvl > w) w = n.lvl;
    }
    weight_ = w;
    for (int s = 0; s < nfr; ++s) {
      out(b, 0, s, acc[s]);
      out(b, 1, s, stereo ? accr[s] : acc[s]);
    }
  }

  int slices() const { int n = p_[8] / 64; return n > 64 ? 64 : n; }
  // the note's range: a zone's offset..end (STA / END within it), or STA..END of the sample (a slice of it); its loop
  void start(const samples::View& v) {
    old_pos_ = pos_;
    old_dir_ = dir_;
    const float last = (float)(v.frames - 1);
    const float a0 = 0.0f, a1 = last;
    float s = a0 + (float)p_[3] / 16383.0f * (a1 - a0), e = a0 + (float)p_[4] / 16383.0f * (a1 - a0);
    if (int n = slices()) {   // slice k of n of s..e (in the play direction)
      int k = (p_[9] / 64 + note_ - 60) % n;
      if (k < 0) k += n;
      float a = s + (e - s) * (float)k / (float)n, z = s + (e - s) * (float)(k + 1) / (float)n;
      s = a;
      e = z;
    }
    dir_ = e >= s ? 1.0f : -1.0f;
    lo_ = s < e ? s : e;
    hi_ = s < e ? e : s;
    llo_ = lo_;
    lhi_ = hi_;
    loop_ = p_[5] >= 64;
    pos_ = s;
  }
  static void read(const samples::View& v, float pos, float* l, float* r) {
    if (pos < 0.0f) pos = 0.0f;
    uint32_t i = (uint32_t)pos;
    if (i >= v.frames - 1) i = v.frames - 2;
    float fr = pos - (float)i;
    if (fr > 1.0f) fr = 1.0f;
    const int16_t* a = v.pcm + (size_t)i * v.ch;
    const int16_t* c = a + v.ch;
    float x0 = a[0] + fr * (c[0] - a[0]);
    float x1 = v.ch > 1 ? a[1] + fr * (c[1] - a[1]) : x0;
    *l = x0 * (1.0f / 32768.0f);
    *r = x1 * (1.0f / 32768.0f);
  }
};

}  // namespace dsp
}  // namespace core
