// DAISYSP: one percussion voice on DaisySP's physical models (Electrosmith,
// MIT). Shared by the simulator (daisy_c.cc) and the Teensy sketch
// (teensy/voiceloop), so the two sound the same. The Rytm's DAISYSP machine
// (rytm-mods 0021, machine 51) sends its knobs as P1..P8:
//   ENG  model: MODAL STRING RESON, in three zones of 0..127 (rytm-mods 0021's
//        curated list, 2026-10-09: model = (ENG * 3) >> 7, ENG the knob's 0..127;
//        DSP SAMPLE's ENG alike; DaisySP's DRIP left out)
//   STR  structure (the modes' spread: bar, plate, bell, string stiffness)
//   BRI  brightness
//   DMP  damping: up = shorter (DaisySP's damping is the other way round)
//   INP  what strikes it: INT (the built-in strike below), or an analog voice's
//        audio (its Overbridge output, BD..CY/CB), which then rings MODAL,
//        STRING and RESON continuously
//   EXC  with INT: MODAL/STRING a noise strike mixed in, RESON the strike's
//        length (a click to a 40 ms noise burst);
//        with an analog input: the input's gain
// The strike's strength (DaisySP's accent) follows the velocity.
//   TUN  semitones from 64; LEV level
#pragma once
#include <math.h>
#include <stdint.h>

#include <stdlib.h>

#include "Filters/onepole.h"
#include "Filters/svf.h"
#include "Utility/dcblock.h"
#include "Utility/delayline.h"

namespace rytm {

// DaisySP's String (Karplus-Strong, "lite" Rings string; Ben Sergentanis /
// Emilie Gillet, MIT) with its parameter math (two powf, an atanf, a tanf)
// done once per block in Prepare() instead of every sample: the same
// arithmetic on the same values per sample, a fraction of the cost.
class BlockString {
 public:
  enum { kDelayLineSize = 1024 };
  void Init(float sample_rate) {
    sample_rate_ = sample_rate;
    string_.Init();
    stretch_.Init();
    Reset();
    SetFreq(440.0f);
    SetDamping(0.8f);
    SetNonLinearity(0.1f);
    SetBrightness(0.5f);
  }
  void Reset() {
    string_.Reset();
    stretch_.Reset();
    iir_damping_filter_.Init();
    dc_blocker_.Init(sample_rate_);
    dispersion_noise_ = curved_bridge_ = 0.0f;
    out_sample_[0] = out_sample_[1] = 0.0f;
    src_phase_ = 0.0f;
  }
  void SetFreq(float freq) { frequency_ = clamp(freq / sample_rate_, 0.0f, 0.25f); }
  void SetNonLinearity(float a) { non_linearity_amount_ = clamp(a, 0.0f, 1.0f); }   // as DaisySP: 0..1
  void SetBrightness(float b) { brightness_ = clamp(b, 0.0f, 1.0f); }
  void SetDamping(float d) { damping_ = clamp(d, 0.0f, 1.0f); }

  // After the Set*s, before a block's Process calls.
  void Prepare() {
    curved_ = non_linearity_amount_ <= 0.0f;
    float nl = curved_ ? -non_linearity_amount_ : non_linearity_amount_;
    float brightness = brightness_;
    float delay = clamp(1.0f / frequency_, 4.0f, kDelayLineSize - 4.0f);
    src_ratio_ = delay * frequency_;
    full_rate_ = src_ratio_ >= 0.9999f;
    if (full_rate_) src_ratio_ = 1.0f;
    float damping_cutoff = fminf(12.0f + damping_ * damping_ * 60.0f + brightness * 24.0f, 84.0f);
    float damping_f = fminf(frequency_ * powf(2.0f, damping_cutoff * (1.0f / 12.0f)), 0.499f);
    if (damping_ >= 0.95f) {   // crossfade to infinite decay
      float to_infinite = 20.0f * (damping_ - 0.95f);
      brightness += to_infinite * (1.0f - brightness);
      damping_f += to_infinite * (0.4999f - damping_f);
      damping_cutoff += to_infinite * (128.0f - damping_cutoff);
    }
    iir_damping_filter_.SetFrequency(damping_f);
    float ratio = powf(2.0f, damping_cutoff * (1.0f / 12.0f));
    float damping_compensation = 1.0f - 2.0f * atanf(1.0f / ratio) / (2.0f * 3.14159265358979f);
    stretch_point_ = nl * (2.0f - nl) * 0.225f;
    stretch_correction_ = clamp((160.0f / sample_rate_) * delay, 1.0f, 2.1f);
    float noise_amount_sqrt = nl > 0.75f ? 4.0f * (nl - 0.75f) : 0.0f;
    noise_amount_ = noise_amount_sqrt * noise_amount_sqrt * 0.1f;
    noise_filter_ = 0.06f + 0.94f * brightness * brightness;
    bridge_curving_ = nl * nl * 0.01f;
    ap_gain_ = -0.618f * nl / (0.15f + fabsf(nl));
    delay_ = delay * damping_compensation;
  }

  float Process(float in) {
    if (full_rate_) src_phase_ = 1.0f;
    src_phase_ += src_ratio_;
    if (src_phase_ > 1.0f) {
      src_phase_ -= 1.0f;
      float delay = delay_, s;
      if (!curved_) {   // dispersion
        float noise = rand() * (1.0f / (float)RAND_MAX) - 0.5f;
        dispersion_noise_ += noise_filter_ * (noise - dispersion_noise_);
        delay *= 1.0f + dispersion_noise_ * noise_amount_;
        float ap_delay = delay * stretch_point_;
        float main_delay = delay - ap_delay * (0.408f - stretch_point_ * 0.308f) * stretch_correction_;
        if (ap_delay >= 4.0f && main_delay >= 4.0f) {
          s = string_.Read(main_delay);
          s = stretch_.Allpass(s, ap_delay, ap_gain_);
        } else {
          s = string_.ReadHermite(delay);
        }
      } else {
        delay *= 1.0f - curved_bridge_ * bridge_curving_;
        s = string_.ReadHermite(delay);
        float value = fabsf(s) - 0.025f;
        float sign = s > 0.0f ? 1.0f : -1.5f;
        curved_bridge_ = (fabsf(value) + value) * sign;
      }
      s = clamp(s + in, -20.0f, 20.0f);
      s = dc_blocker_.Process(s);
      s = iir_damping_filter_.Process(s);
      string_.Write(s);
      out_sample_[1] = out_sample_[0];
      out_sample_[0] = s;
    }
    return out_sample_[1] * (1.0f - src_phase_) + out_sample_[0] * src_phase_;
  }

 private:
  static float clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
  daisysp::DelayLine<float, kDelayLineSize> string_;
  daisysp::DelayLine<float, kDelayLineSize / 4> stretch_;
  float frequency_ = 0.01f, non_linearity_amount_ = 0.1f, brightness_ = 0.5f, damping_ = 0.8f;
  float sample_rate_ = 48000.0f;
  daisysp::OnePole iir_damping_filter_;
  daisysp::DcBlock dc_blocker_;
  float dispersion_noise_ = 0.0f, curved_bridge_ = 0.0f, src_phase_ = 0.0f, out_sample_[2] = {0, 0};
  // Prepare()'s
  bool curved_ = false, full_rate_ = true;
  float src_ratio_ = 1.0f, delay_ = 100.0f, stretch_point_ = 0.0f, stretch_correction_ = 1.0f;
  float noise_amount_ = 0.0f, noise_filter_ = 0.0f, bridge_curving_ = 0.0f, ap_gain_ = 0.0f;
};

// DaisySP's StringVoice (MIT) on BlockString: the strike (a noise burst
// through a low-pass) into the string; brightness and damping lifted by the
// accent, per block (they are constant within one).
class BlockStringVoice {
 public:
  void Init(float sample_rate) {
    sample_rate_ = sample_rate;
    excitation_filter_.Init(sample_rate);
    string_.Init(sample_rate);
    remaining_noise_samples_ = 0;
    f0_ = 440.0f / sample_rate;
    string_.SetFreq(440.0f);
    accent_ = 0.8f;
    SetStructure(0.7f);
    brightness_ = 0.2f;
    damping_ = 0.7f;
  }
  void SetFreq(float freq) {
    string_.SetFreq(freq);
    f0_ = freq / sample_rate_;
    f0_ = f0_ < 0.0f ? 0.0f : f0_ > 0.25f ? 0.25f : f0_;
  }
  void SetAccent(float a) { accent_ = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a; }
  void SetStructure(float structure) {
    structure = structure < 0.0f ? 0.0f : structure > 1.0f ? 1.0f : structure;
    string_.SetNonLinearity(structure < 0.24f ? (structure - 0.24f) * 4.166f
                            : structure > 0.26f ? (structure - 0.26f) * 1.35135f : 0.0f);
  }
  void SetBrightness(float b) { brightness_ = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b; }
  void SetDamping(float d) { damping_ = d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d; }
  // After the Set*s, before a block's Process calls.
  void Prepare() {
    brightness_now_ = brightness_ + 0.25f * accent_ * (1.0f - brightness_);
    string_.SetBrightness(brightness_now_);
    string_.SetDamping(damping_ + 0.25f * accent_ * (1.0f - damping_));
    string_.Prepare();
  }
  // The string itself: STRING rung by outside audio plays it too (one string
  // a voice, 5 KB, whichever way it is excited).
  BlockString& string() { return string_; }

  float Process(bool trigger) {
    if (trigger) {
      const float b = brightness_now_;
      const float cutoff = fminf(4.0f * f0_ * powf(2.0f, (1.0f / 12.0f) * (b * (2.0f - b) - 0.5f) * 72.0f), 0.499f);
      remaining_noise_samples_ = (size_t)(1.0f / f0_);
      excitation_filter_.SetFreq(cutoff * sample_rate_);
      excitation_filter_.SetRes(0.5f);
    }
    float temp = 0.0f;
    if (remaining_noise_samples_) {
      temp = 2.0f * rand() * (1.0f / (float)RAND_MAX) - 1.0f;
      remaining_noise_samples_--;
    }
    excitation_filter_.Process(temp);
    return string_.Process(excitation_filter_.Low());
  }

 private:
  float sample_rate_ = 48000.0f, f0_ = 0.01f, accent_ = 0.8f, brightness_ = 0.2f, damping_ = 0.7f;
  float brightness_now_ = 0.2f;
  size_t remaining_noise_samples_ = 0;
  daisysp::Svf excitation_filter_;
  BlockString string_;
};

// DaisySP's Resonator (from Plaits' modal resonator, Emilie Gillet, MIT) with
// its 24 modes' coefficients computed once per block instead of every sample:
// the same filters, about a quarter of the cost on the Teensy. Prepare() takes
// f0 in cycles per sample.
class BlockResonator {
 public:
  enum { kModes = 24 };
  void Init(float position) {
    float a = cosf(position * 6.2831853f) * 0.25f;
    for (int i = 0; i < kModes; ++i) amp_[i] = a, s1_[i] = s2_[i] = 0.0f;
  }
  void Prepare(float f0, float structure, float brightness, float damping) {
    float stiffness = Stiffness(structure);
    f0 *= HarmonicCompensation(stiffness);
    float harmonic = f0, stretch = 1.0f;
    float q_sqrt = powf(2.0f, damping * 79.7f / 12.0f);
    float q = 500.0f * q_sqrt * q_sqrt;
    brightness *= 1.0f - structure * 0.3f;
    brightness *= 1.0f - damping * 0.3f;
    float q_loss = brightness * (2.0f - brightness) * 0.85f + 0.15f;
    for (int i = 0; i < kModes; ++i) {
      float f = harmonic * stretch;
      if (f >= 0.499f) f = 0.499f;
      float g = FastTan(f), r = 1.0f / (1.0f + f * q);
      g_[i] = g;
      rg_[i] = r + g;
      h_[i] = 1.0f / (1.0f + r * g + g * g);
      gain_[i] = amp_[i] * (1.0f - f * 2.0f);
      stretch += stiffness;
      stiffness *= stiffness < 0.0f ? 0.93f : 0.98f;
      harmonic += f0;
      q *= q_loss;
    }
  }
  // Four modes at a time, their steps interleaved: the Teensy's in-order
  // core then overlaps one mode's latencies with the others' work (the same
  // arithmetic per mode, the sum in mode order, as one at a time).
  inline float Process(float in) {
    float out = 0.0f;
    for (int i = 0; i < kModes; i += 4) {
      float a1 = s1_[i], b1 = s1_[i + 1], c1 = s1_[i + 2], d1 = s1_[i + 3];
      float a2 = s2_[i], b2 = s2_[i + 1], c2 = s2_[i + 2], d2 = s2_[i + 3];
      float ag = g_[i], bg = g_[i + 1], cg = g_[i + 2], dg = g_[i + 3];
      float ahp = (in - rg_[i] * a1 - a2) * h_[i];
      float bhp = (in - rg_[i + 1] * b1 - b2) * h_[i + 1];
      float chp = (in - rg_[i + 2] * c1 - c2) * h_[i + 2];
      float dhp = (in - rg_[i + 3] * d1 - d2) * h_[i + 3];
      float abp = ag * ahp + a1, bbp = bg * bhp + b1, cbp = cg * chp + c1, dbp = dg * dhp + d1;
      s1_[i] = ag * ahp + abp;
      s1_[i + 1] = bg * bhp + bbp;
      s1_[i + 2] = cg * chp + cbp;
      s1_[i + 3] = dg * dhp + dbp;
      float alp = ag * abp + a2, blp = bg * bbp + b2, clp = cg * cbp + c2, dlp = dg * dbp + d2;
      s2_[i] = ag * abp + alp;
      s2_[i + 1] = bg * bbp + blp;
      s2_[i + 2] = cg * cbp + clp;
      s2_[i + 3] = dg * dbp + dlp;
      out += gain_[i] * abp;
      out += gain_[i + 1] * bbp;
      out += gain_[i + 2] * cbp;
      out += gain_[i + 3] * dbp;
    }
    return out;
  }
  static float FastTan(float f) {
    const float pi = 3.14159265f, a = 3.260e-01f * pi * pi * pi, b = 1.823e-01f * pi * pi * pi * pi * pi;
    float f2 = f * f;
    return f * (pi + f2 * (a + b * f2));
  }

 private:
  static float Stiffness(float sig) {
    if (sig < 0.25f) return -(0.25f - sig) * 0.25f;
    if (sig < 0.3f) return 0.0f;
    if (sig < 0.9f) return (sig - 0.3f) / 0.6f;
    sig = (sig - 0.9f) * 10.0f;
    sig *= sig;
    return 1.5f - cosf(sig * 3.14159265f) * 0.5f;
  }
  static float HarmonicCompensation(float stiffness) {   // the 3rd harmonic's
    float stretch = 1.0f;
    for (int i = 0; i < 2; ++i) {
      stretch += stiffness;
      stiffness *= stiffness < 0.0f ? 0.93f : 0.98f;
    }
    return 1.0f / stretch;
  }
  float amp_[kModes], g_[kModes], rg_[kModes], h_[kModes], gain_[kModes];
  float s1_[kModes], s2_[kModes];
};

class DaisyEngine {
 public:
  enum Model { MODAL, STRING, RESON, MODELS };

  void Init(float sample_rate) {
    sr_ = sample_rate;
    modal_.Init(0.015f);
    string_.Init(sample_rate);
    reson_.Init(0.015f);
    ex1_ = ex2_ = 0.0f;
    noise_ = 0x12345678u;
    burst_ = burst_env_ = 0.0f;
  }

  // model 0..2 (MODAL STRING RESON); note in semitones (MIDI numbering); the rest 0..1. `trigger`
  // starts a strike on the first sample. `in` (n samples, or null): an analog
  // voice's audio to excite it instead, with EXC as its gain. Writes n floats,
  // about -1..1.
  void Render(int model, float note, float structure, float brightness,
              float damping, float accent, float exciter, bool trigger,
              float level, float* out, int n, const float* in = nullptr) {
    if (model < 0) model = 0;
    if (model >= MODELS) model = MODELS - 1;
    damping = 1.0f - damping;
    float f = 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
    if (f > sr_ * 0.24f) f = sr_ * 0.24f;
    float acc = accent * level;
    // the input's gain: EXC 0..1 -> 0..16 (an analog voice arrives quiet)
    float in_gain = 16.0f * exciter * exciter;
    if (in) {
      RenderInput(model, f, structure, brightness, damping, in, in_gain, level, out, n);
      return;
    }
    if (trigger) {
      // the strike noise: EXC sets its length (MODAL/STRING/RESON)
      burst_ = 1.0f;
      float ms = 0.5f + 40.0f * exciter * exciter;
      burst_env_ = expf(-1.0f / (ms * 0.001f * sr_));
    }
    switch (model) {
      case MODAL: {
        // DaisySP's ModalVoice (struck, not sustained): an impulse through a
        // low-pass into the resonator, brightness and damping lifted by accent.
        float f0 = f / sr_;
        if (f0 > 0.25f) f0 = 0.25f;
        float a = acc > 1.0f ? 1.0f : acc;
        float bri = brightness + 0.25f * a * (1.0f - brightness);
        float dmp = damping + 0.25f * a * (1.0f - damping);
        float cutoff = 2.0f * f0 * powf(2.0f, ((bri * (2.0f - bri) - 0.5f) * 60.0f) / 12.0f);
        if (cutoff > 0.499f) cutoff = 0.499f;
        float g = BlockResonator::FastTan(cutoff), r = 1.0f / 1.5f;
        float h = 1.0f / (1.0f + r * g + g * g), rg = r + g;
        float strike = 0.0f;
        if (trigger) {
          float amplitude = (0.12f + 0.08f * a) * (1.0f - dmp * 0.5f);
          strike = amplitude * powf(2.0f, cutoff * cutoff * 24.0f / 12.0f) / cutoff;
        }
        modal_.Prepare(f0, structure, bri, dmp);
        for (int i = 0; i < n; ++i) {
          float in = i == 0 ? strike : 0.0f;
          float hp = (in - rg * ex1_ - ex2_) * h;   // the excitation low-pass
          float bp = g * hp + ex1_;
          ex1_ = g * hp + bp;
          float lp = g * bp + ex2_;
          ex2_ = g * bp + lp;
          out[i] = (modal_.Process(lp) + Strike() * exciter * acc * 0.5f) * level;
        }
        break;
      }
      case STRING:
        string_.SetFreq(f);
        string_.SetStructure(structure);
        string_.SetBrightness(brightness);
        string_.SetDamping(damping);
        string_.SetAccent(acc);
        string_.Prepare();
        for (int i = 0; i < n; ++i) {
          float s = string_.Process(trigger && i == 0);
          out[i] = (s + Strike() * exciter * acc * 0.5f) * level * 1.5f;
        }
        break;
      default: {   // RESON
        float f0 = f / sr_;
        reson_.Prepare(f0 > 0.25f ? 0.25f : f0, structure, brightness, damping);
        for (int i = 0; i < n; ++i) {
          // a click on the first sample, then the burst
          float in = (trigger && i == 0 ? 1.0f : 0.0f) + Strike() * 0.25f;
          out[i] = reson_.Process(in * (0.2f + acc)) * level * 0.6f;
        }
        break;
      }
    }
  }

 private:
  // MODAL, STRING, RESON rung by outside audio, no strike of their own.
  void RenderInput(int model, float f, float structure, float brightness, float damping,
                   const float* in, float gain, float level, float* out, int n) {
    float f0 = f / sr_;
    if (f0 > 0.25f) f0 = 0.25f;
    if (model == STRING) {
      // DaisySP's String (Karplus-Strong) takes an input; StringVoice's
      // structure -> non-linearity mapping
      float st = structure < 0.0f ? 0.0f : structure > 1.0f ? 1.0f : structure;
      BlockString& ks = string_.string();
      ks.SetFreq(f);
      ks.SetNonLinearity(st < 0.24f ? (st - 0.24f) * 4.166f : st > 0.26f ? (st - 0.26f) * 1.35135f : 0.0f);
      ks.SetBrightness(brightness);
      ks.SetDamping(damping);
      ks.Prepare();
      for (int i = 0; i < n; ++i) out[i] = ks.Process(in[i] * gain) * level;
      return;
    }
    BlockResonator& r = model == MODAL ? modal_ : reson_;
    r.Prepare(f0, structure, brightness, damping);
    for (int i = 0; i < n; ++i) out[i] = r.Process(in[i] * gain) * level * 0.6f;
  }

  // Noise times a decaying envelope, white noise from a 32-bit LCG.
  float Strike() {
    if (burst_ < 1e-4f) return 0.0f;
    noise_ = noise_ * 1664525u + 1013904223u;
    float w = (float)(int32_t)noise_ * (1.0f / 2147483648.0f);
    float s = w * burst_;
    burst_ *= burst_env_;
    return s;
  }

  float sr_ = 48000.0f;
  BlockResonator modal_;
  float ex1_ = 0.0f, ex2_ = 0.0f;   // MODAL's excitation filter
  BlockStringVoice string_;
  BlockResonator reson_;
  uint32_t noise_ = 0;
  float burst_ = 0.0f, burst_env_ = 0.0f;
};

}  // namespace rytm
