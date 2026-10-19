// A Juno-106 voice in float, for the JUNO machine (rytm-mods 0021), the
// simulator and the Teensy alike. The signal path and the curves are AMY's
// Juno-106 model (shorepine/amy amy/juno.py, MIT): its 128 factory patches as
// the 106's 18 sysex bytes (juno_patches.h) and how each slider maps to time,
// frequency and level; the DSP here is our own.
//
//   LFO  triangle, rate 0.5..20 Hz, delayed fade-in after a note
//   DCO  saw, pulse (width by hand or by the LFO), a square one octave down
//        (SUB), noise; 16'/8'/4'; LFO pitch modulation
//   HPF  the 106's four positions: bass boost, flat, two high-passes
//   VCF  4-pole low-pass ladder (zero-delay feedback), resonance, keyboard
//        tracking, envelope (+ or -) and LFO amounts in octaves
//   VCA  the envelope, or a gate; level
//   ENV  one ADSR for VCF and VCA (A linear, D/R exponential: AMY's times are
//        to e^-3 of the distance)
//   CHO  mono chorus I, II, I+II (the Rytm's voice is one channel)
//
// Rytm knobs (the host's P1..P8): PAT the patch; CUT RES ENV ATK DEC REL the
// sliders themselves, absolute 0..127 (2026-10-09), in place of the patch's
// VCF FREQ, RES, ENV, A, D and R (bytes 5, 6, 7, 11, 12, 14; the other bytes
// are the patch's) - the host sends a patch's own values to the Rytm's knobs
// when PAT picks another patch (15, sim/protocol.py), and plays the patch's
// own until the knobs carry them; LEN the gate. A
// patched Rytm sends a note-off (cmd 0x11, 2026-10-09) when the voice's gate
// closes - pad released, the trig's LEN elapsed, the sequencer stopped:
// LEN 127 follows the Rytm's note length (held until the note-off; with older
// firmware, which sends none, until the next trig); LEN < 127 a timed gate,
// 20 ms * 2^(LEN/14), released at the end of it or at the note-off, whichever
// comes first.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "juno_patches.h"

namespace juno {

class Voice {
 public:
  static const int kChorusLen = 512;   // ~10.7 ms at 48 kHz
  static const int kSub = 4;           // control rate: envelope, cutoff, gain

  void Init(float sample_rate) {
    sr_ = sample_rate;
    phase_ = sub_phase_ = lfo_phase_ = 0.0f;
    for (int i = 0; i < 4; ++i) s_[i] = 0.0f;
    hp_s_ = shelf_s_ = 0.0f;
    env_ = 0.0f;
    stage_ = kOff;
    lfo_amp_ = 0.0f;
    noise_ = 0x12345678u;
    for (int i = 0; i < kChorusLen; ++i) cho_[i] = 0.0f;
    cho_w_ = 0;
    cho_phase_ = 0.0f;
    patch_ = -1;
    for (int i = 0; i < 8; ++i) knob_[i] = 64;
    knob_[0] = 0;
    SetPatch(0);
    Derive();
  }

  // P1..P8 as the Rytm sends them (0..127).
  void SetKnobs(const float* k) {
    int p = (int)k[0];
    p = p < 0 ? 0 : p > 127 ? 127 : p;
    for (int i = 0; i < 8; ++i) knob_[i] = k[i];
    if (!ext_ && p != patch_) SetPatch(p);
    Derive();
  }

  // A patch from elsewhere - the 106's 18 sysex bytes and a name (the
  // project's JUNO list on the card) - instead of the factory one PAT picks;
  // nullptr: PAT picks a factory patch again. SetKnobs after it.
  void UsePatch(const uint8_t* b, const char* name) {
    if (b) {
      memcpy(b_, b, 18);
      gain_ = 1.0f;
      ext_ = true;
      strncpy(ext_name_, name ? name : "", sizeof ext_name_ - 1);
      ext_name_[sizeof ext_name_ - 1] = 0;
    } else if (ext_) {
      ext_ = false;
      patch_ = -1;
    }
  }

  // A trig: note (MIDI, fractional), velocity 0..1.
  void NoteOn(float note, float velocity) {
    note_ = note;
    vel_ = velocity;
    stage_ = kAttack;           // from where the envelope is: no click
    lfo_amp_ = 0.0f;
    held_ = 0;
    float k = knob_[7];
    gate_len_ = k >= 127.0f ? 0 : (uint32_t)(sr_ * 0.02f * exp2f(k / 14.0f));
  }

  // The Rytm's note-off: the release, from wherever the envelope is (also
  // mid-attack). Nothing when it is already releasing or silent.
  void NoteOff() {
    if (stage_ != kRelease && stage_ != kOff) stage_ = kRelease;
  }

  const char* name() const { return ext_ ? ext_name_ : kPatches[patch_ < 0 ? 0 : patch_].name; }

  void Render(float* out, int n) {
    if (stage_ == kOff && amp_ == 0.0f) {
      float d = 0.0035f * sr_;
      for (int i = 0; i < n; ++i) out[i] = Chorus(0.0f, d);
      return;
    }
    // pitch, pulse width, LFO: once a block
    float lfo_inc = lfo_hz_ / sr_;
    float lfo = Triangle(lfo_phase_) * lfo_amp_;
    float pitch = note_ - 60.0f + 12.0f * (oct_ + dco_lfo_ * 0.03f * lfo);
    float f = 261.63f * exp2f(pitch / 12.0f);
    float inc = f / sr_;
    float duty = duty_const_ + duty_lfo_ * lfo;
    duty = duty < 0.02f ? 0.02f : duty > 0.98f ? 0.98f : duty;
    float oct0 = vcf_lfo_oct_ * lfo + kbd_ * (note_ - 60.0f) / 12.0f;
    float fmax = 0.45f * sr_;
    float cho_d = 0.0f;
    if (chorus_) {
      cho_phase_ += cho_rate_ * n / sr_;
      if (cho_phase_ >= 1.0f) cho_phase_ -= 1.0f;
      cho_d = 0.0035f * sr_ + cho_depth_ * Triangle(cho_phase_);
    }
    // envelope, cutoff and gain every kSub samples: a strong, fast filter
    // envelope stepped once a 12-sample block clicked [user report 2026-10-09]
    for (int i0 = 0; i0 < n; i0 += kSub) {
      int m = n - i0 < kSub ? n - i0 : kSub;
      EnvBlock(m);
      float fc = vcf_hz_ * exp2f(oct0 + vcf_env_oct_ * env_);
      fc = fc < 10.0f ? 10.0f : fc > fmax ? fmax : fc;
      float g = tanf(3.14159265f * fc / sr_);
      float inv1g = 1.0f / (1.0f + g);
      float G = g * inv1g;
      float G2 = G * G, G3 = G2 * G, G4 = G2 * G2;
      float inv_u = 1.0f / (1.0f + k_ * G4);
      // VCA: the envelope, or the gate (a ~1.5 ms ramp, not a step)
      float target;
      if (vca_gate_) {
        bool open = stage_ != kRelease && stage_ != kOff;
        gate_amp_ += ((open ? 1.0f : 0.0f) - gate_amp_) * gate_k_;
        target = gate_amp_;
      } else {
        target = env_;
      }
      float amp1 = level_ * vel_ * target;
      float amp = amp_, damp = (amp1 - amp_) / m;
      amp_ = amp1;
      for (int i = i0; i < i0 + m; ++i) {
        // DCO
        float x = 0.0f;
        if (saw_) x += 2.0f * phase_ - 1.0f - Blep(phase_, inc);
        if (pulse_) {
          float t = phase_ - duty;
          if (t < 0.0f) t += 1.0f;
          x += (phase_ < duty ? 1.0f : -1.0f) + Blep(phase_, inc) - Blep(t, inc);
        }
        if (sub_ > 0.0f) {
          float t = sub_phase_ - 0.5f;
          if (t < 0.0f) t += 1.0f;
          x += sub_ * ((sub_phase_ < 0.5f ? 1.0f : -1.0f) + Blep(sub_phase_, inc * 0.5f) -
                       Blep(t, inc * 0.5f));
        }
        if (noise_lvl_ > 0.0f) {
          noise_ ^= noise_ << 13;
          noise_ ^= noise_ >> 17;
          noise_ ^= noise_ << 5;
          x += noise_lvl_ * ((int32_t)noise_ * (1.0f / 2147483648.0f));
        }
        phase_ += inc;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        sub_phase_ += inc * 0.5f;
        if (sub_phase_ >= 1.0f) sub_phase_ -= 1.0f;
        x = x * 0.35f + 1e-5f * ((int32_t)(noise_ * 2654435761u) * (1.0f / 2147483648.0f));
        // HPF: one-pole high-pass, or a low shelf for the bass boost
        float lp = hp_s_ + hp_a_ * (x - hp_s_);
        hp_s_ = lp;
        x = x - hp_cut_ * lp + hp_boost_ * lp;
        // VCF: 4-pole ladder, zero-delay feedback
        float S = (G3 * s_[0] + G2 * s_[1] + G * s_[2] + s_[3]) * inv1g;
        float u = (x - k_ * S) * inv_u;
        u = u < -2.0f ? -2.0f : u > 2.0f ? 2.0f : u;
        u = u - u * u * u * (1.0f / 13.5f);   // a little saturation into the ladder
        float y = u;
        for (int j = 0; j < 4; ++j) {
          float v = (y - s_[j]) * G;
          float o = v + s_[j];
          s_[j] = o + v;
          y = o;
        }
        out[i] = Chorus(y * amp, cho_d);
        amp += damp;
      }
    }
    lfo_phase_ += lfo_inc * n;
    if (lfo_phase_ >= 1.0f) lfo_phase_ -= floorf(lfo_phase_);
    if (lfo_amp_ < 1.0f) {
      lfo_amp_ += n / (sr_ * lfo_delay_s_);
      if (lfo_amp_ > 1.0f) lfo_amp_ = 1.0f;
    }
  }

 private:
  enum Stage { kOff, kAttack, kDecay, kSustain, kRelease };

  void SetPatch(int p) {
    patch_ = p;
    for (int i = 0; i < 18; ++i) b_[i] = kPatches[p].b[i];
    gain_ = 1.0f;   // AMY's trims (kPatches[p].gain) suit its own mix, not this voice
  }

  // A slider, 0..1: the patch's byte, or its knob's value (absolute) when it
  // has one.
  float Slider(int byte, int knob) const {
    float v = knob >= 0 ? knob_[knob] : b_[byte];
    return (v < 0.0f ? 0.0f : v > 127.0f ? 127.0f : v) / 127.0f;
  }

  void Derive() {
    float lfo_rate = Slider(0, -1), lfo_delay = Slider(1, -1);
    lfo_hz_ = 0.6f * exp2f(0.04f * lfo_rate * 127.0f) - 0.1f;
    lfo_delay_s_ = (18.0f * exp2f(0.066f * lfo_delay * 127.0f) - 13.0f) / 1000.0f;
    if (lfo_delay_s_ < 0.005f) lfo_delay_s_ = 0.005f;
    dco_lfo_ = Slider(2, -1);
    float pwm = Slider(3, -1);
    noise_lvl_ = Slider(4, -1);
    float vcf = Slider(5, 1), res = Slider(6, 2), env = Slider(7, 3);
    vcf_lfo_oct_ = 1.25f * Slider(8, -1);
    kbd_ = Slider(9, -1);
    // VCA LEVEL: about -10..+10 dB, as the 106's slider (AMY's linear map
    // makes 0 silent: B23 and B28 would not sound)
    level_ = 0.6f * exp2f(3.32f * (Slider(10, -1) - 0.5f)) * gain_;
    float a = Slider(11, 4), d = Slider(12, 5), s = Slider(13, -1), r = Slider(14, 6);
    sub_ = Slider(15, -1);
    int b16 = b_[16], b17 = b_[17];
    bool stop16 = b16 & 1, stop4 = b16 & 4;
    pulse_ = (b16 >> 3) & 1;
    saw_ = (b16 >> 4) & 1;
    static const int kChorus[4] = {2, 0, 1, 0};
    chorus_ = kChorus[(b16 >> 5) & 3];
    bool pwm_manual = b17 & 1, vcf_neg = b17 & 2;
    vca_gate_ = b17 & 4;
    static const int kHpf[4] = {3, 2, 1, 0};
    int hpf = kHpf[(b17 >> 3) & 3];
    oct_ = stop16 ? -1.0f : stop4 ? 1.0f : 0.0f;
    duty_const_ = 0.5f + 0.5f * (pwm_manual ? pwm : 0.0f);
    duty_lfo_ = 0.5f * (pwm_manual ? 0.0f : pwm);
    vcf_hz_ = 13.0f * exp2f(0.0938f * vcf * 127.0f) * exp2f(0.75f * kbd_);
    vcf_env_oct_ = 11.0f * env * (vcf_neg ? -1.0f : 1.0f);
    float q = 0.7f * exp2f(4.0f * res);
    k_ = 4.0f * (1.0f - 0.7f / q) * 1.09f;   // past 4 at the top: it self-oscillates (B28)
    attack_s_ = (6.0f + 8.0f * a * 127.0f) / 1000.0f;
    decay_s_ = (80.0f * exp2f(0.085f * d * 127.0f) - 80.0f) / 1000.0f;
    release_s_ = (70.0f * exp2f(0.066f * r * 127.0f) - 70.0f) / 1000.0f;
    sustain_ = s;
    att_inc_ = 1.0f / (attack_s_ * sr_);
    dec_k_ = decay_s_ > 0.0005f ? expf(-3.0f / (decay_s_ * sr_)) : 0.0f;
    rel_k_ = release_s_ > 0.0005f ? expf(-3.0f / (release_s_ * sr_)) : 0.0f;
    dec_ks_ = powf(dec_k_, (float)kSub);
    rel_ks_ = powf(rel_k_, (float)kSub);
    gate_k_ = 1.0f - expf(-(float)kSub / (0.0015f * sr_));
    // HPF: 0 bass boost (+7 dB shelf), 1 flat, 2 / 3 high-pass ~240 / ~720 Hz
    static const float kHz[4] = {150.0f, 150.0f, 240.0f, 720.0f};
    hp_a_ = 1.0f - expf(-6.2831853f * kHz[hpf] / sr_);
    hp_cut_ = hpf >= 2 ? 1.0f : 0.0f;
    hp_boost_ = hpf == 0 ? 1.24f : 0.0f;   // +7 dB below the corner
    // chorus I 0.5 Hz, II 0.83 Hz, I+II 1 Hz shallow (AMY's settings)
    static const float kRate[4] = {0.0f, 0.5f, 0.83f, 1.0f};
    static const float kDepth[4] = {0.0f, 1.6f, 1.6f, 0.3f};   // ms
    cho_rate_ = kRate[chorus_];
    cho_depth_ = kDepth[chorus_] * 0.001f * sr_;
  }

  // n samples of the envelope at once (the per-sample constants to the n).
  void EnvBlock(int n) {
    if (gate_len_ && stage_ != kRelease && stage_ != kOff) {
      held_ += n;
      if (held_ >= gate_len_) stage_ = kRelease;
    }
    switch (stage_) {
      case kAttack:
        env_ += att_inc_ * n;
        if (env_ >= 1.0f) {
          env_ = 1.0f;
          stage_ = kDecay;
        }
        break;
      case kDecay:
        env_ = sustain_ + (env_ - sustain_) * (n == kSub ? dec_ks_ : powf(dec_k_, (float)n));
        if (env_ - sustain_ < 0.0005f) stage_ = kSustain;
        break;
      case kSustain:
        env_ = sustain_;
        break;
      case kRelease:
        env_ *= (n == kSub ? rel_ks_ : powf(rel_k_, (float)n));
        if (env_ < 0.0001f) {
          env_ = 0.0f;
          stage_ = kOff;
        }
        break;
      default:
        break;
    }
  }

  float Chorus(float x, float d) {
    if (!chorus_) return x;
    cho_[cho_w_] = x;
    float rp = (float)cho_w_ - d;
    if (rp < 0.0f) rp += kChorusLen;
    int i0 = (int)rp;
    float fr = rp - i0;
    int i1 = i0 + 1 == kChorusLen ? 0 : i0 + 1;
    float wet = cho_[i0] + (cho_[i1] - cho_[i0]) * fr;
    if (++cho_w_ == kChorusLen) cho_w_ = 0;
    return 0.6f * x + 0.6f * wet;
  }

  static float Triangle(float p) { return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p; }

  // PolyBLEP: the step at phase 0 of a ramp advancing inc a sample.
  static float Blep(float t, float inc) {
    if (t < inc) {
      t /= inc;
      return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - inc) {
      t = (t - 1.0f) / inc;
      return t * t + t + t + 1.0f;
    }
    return 0.0f;
  }

  float sr_ = 48000.0f;
  int patch_ = -1;
  bool ext_ = false;
  char ext_name_[24] = "";
  uint8_t b_[18];
  float gain_ = 1.0f;
  float knob_[8];
  // derived
  float lfo_hz_, lfo_delay_s_, dco_lfo_, noise_lvl_, vcf_lfo_oct_, kbd_, level_, sub_;
  bool pulse_, saw_, vca_gate_;
  int chorus_;
  float oct_, duty_const_, duty_lfo_, vcf_hz_, vcf_env_oct_, k_;
  float attack_s_, decay_s_, release_s_, sustain_, att_inc_, dec_k_, rel_k_, dec_ks_, rel_ks_, gate_k_;
  float hp_a_, hp_cut_, hp_boost_, cho_rate_, cho_depth_;
  // state
  float note_ = 60.0f, vel_ = 1.0f;
  float phase_, sub_phase_, lfo_phase_, lfo_amp_;
  float s_[4], hp_s_, shelf_s_;
  float env_, amp_ = 0.0f, gate_amp_ = 0.0f;
  Stage stage_;
  uint32_t held_ = 0, gate_len_ = 0;
  uint32_t noise_;
  float cho_[kChorusLen];
  int cho_w_;
  float cho_phase_;
};

}  // namespace juno
