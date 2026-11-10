// core/dsp/midiout.h - MIDIOUT, the DSP library's MIDI sender (2026-11-10, ext's MIDI machine on the library; the
// params follow upstream's MIDI machine, docs/MIDI-MACHINE.md on trigcondition/sidecar-firmware dev, for the USB-A
// destinations the generic engine has): a note it gets (a Rytm voice machine's trig through core/dsp's TRIG, or a
// USB-A device's note) goes out to the USB-A MIDI devices (core/devmidi out) on a channel, with its velocity; the
// values go out when they change. No audio of its own: the input passes through (a graph may chain it).
//
// Params (the voice blocks' scale, value 0..127.98):
//   P0 DST   0 OFF, 1 every USB-A device, 2..5 device 1..4 (plug order)
//   P1 CHN   the channel: value 0..15 is 1..16
//   P2 LEN   the note's length: 0 until the next note; 1..127 10 ms .. 8 s
//   P3 TRN   semitones from 64, added to the note
//   P4 PROG  0 OFF; n program change n - 1
//   P5 PB    0 OFF; 64 centre (pitch bend, value x 128)
//   P6 AT    0 OFF; n channel pressure n
//   P7 MW    0 OFF; n CC 1 n
//   P8 VAL1  P9 SEL1  P10 VAL2  P11 SEL2  P12 VAL3  P13 SEL3  P14 VAL4  P15 SEL4: SELn 0 OFF; else VALn on CC SELn - 1
// A new DST or CHN ends the held note first and sends every value again there. Notes off at link_reset too.
#pragma once
#include "../devmidi/devmidi.h"
#include "blocks.h"

namespace core {
namespace dsp {

class MidiOut : public Block {
 public:
  MidiOut() {
    for (int k = 0; k < kMaxParams; ++k) p_[k] = 0;
    p_[0] = 1 * 64; p_[3] = 64 * 64;
    for (int k = 0; k < kMaxParams; ++k) sent_[k] = -1;
  }
  const char* name() const override { return "MIDIOUT"; }
  void link_reset() override { off(); }
  void midi(int, uint8_t status, uint8_t d1, uint8_t d2) override {
    if ((status & 0xF0) == 0x90 && d2) { pnote_ = d1; pvel_ = d2; pending_ = true; }
  }
  void process(const proc::Block& b) override {
    const int dst = vv(0), ch = vv(1) > 15 ? 15 : vv(1);
    if (dst != dst_ || ch != ch_) {   // a new destination: the held note ends where it played, the values go again
      off();
      dst_ = dst;
      ch_ = ch;
      for (int k = 0; k < kMaxParams; ++k) sent_[k] = -1;
    }
    if (dst_ > 0) {
      values();
      if (pending_) {
        off();
        int n = pnote_ + vv(3) - 64;
        n = n < 0 ? 0 : n > 127 ? 127 : n;
        if (send(0x90, (uint8_t)n, pvel_)) { held_ = n; left_ = len_frames(); }
      }
    }
    pending_ = false;
    if (held_ >= 0 && left_ > 0 && (left_ -= b.frames) <= 0) off();
    for (int f = 0; f < b.frames; ++f) { out(b, 0, f, in(b, 0, f)); out(b, 1, f, in(b, 1, f)); }
  }

 private:
  int dst_ = -1, ch_ = -1, held_ = -1, hdst_ = 0, hch_ = 0, left_ = 0;
  uint8_t pnote_ = 60, pvel_ = 100;
  bool pending_ = false;
  int sent_[kMaxParams];
  int vv(int k) const { return p_[k] / 64; }
  int len_frames() const {
    int v = vv(2);
    return v ? (int)(0.010f * powf(800.0f, (float)(v - 1) / 126.0f) * kRate) : 0;
  }
  bool send(uint8_t st, uint8_t d1, uint8_t d2) { return devmidi::out(dst_ - 1, (uint8_t)(st | ch_), d1, d2); }
  void off() {
    if (held_ >= 0) devmidi::out(hdst_ - 1, (uint8_t)(0x80 | hch_), (uint8_t)held_, 0);
    held_ = -1;
    left_ = 0;
    hdst_ = dst_;
    hch_ = ch_;
  }
  // the values that changed (0: OFF, not sent)
  void values() {
    hdst_ = dst_;
    hch_ = ch_;
    auto changed = [&](int k) { int v = vv(k); if (v == sent_[k]) return false; sent_[k] = v; return v > 0; };
    if (changed(4)) send(0xC0, (uint8_t)(vv(4) - 1), 0);
    if (changed(5)) { int x = vv(5) * 128; x = x > 16383 ? 16383 : x; send(0xE0, (uint8_t)(x & 0x7F), (uint8_t)(x >> 7)); }
    if (changed(6)) send(0xD0, (uint8_t)vv(6), 0);
    if (changed(7)) send(0xB0, 1, (uint8_t)vv(7));
    for (int i = 0; i < 4; ++i) {
      int vk = 8 + 2 * i, sk = 9 + 2 * i;
      bool cv = vv(vk) != sent_[vk], cs = vv(sk) != sent_[sk];
      if (!cv && !cs) continue;
      sent_[vk] = vv(vk);
      sent_[sk] = vv(sk);
      if (vv(sk) > 0) send(0xB0, (uint8_t)(vv(sk) - 1), (uint8_t)vv(vk));
    }
  }
};

}  // namespace dsp
}  // namespace core
