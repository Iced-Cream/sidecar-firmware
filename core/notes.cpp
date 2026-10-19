// core/notes.cpp - see notes.h. Cold (program flash on the Teensy): one call per MIDI message.
#include "notes.h"

#include <string.h>

#include "fx/send_fx.h"
#include "platform.h"
#include "voices.h"

namespace core {

namespace {
struct Mach { const char* name; uint8_t flag; uint8_t p[16]; };
const Mach kMachs[] = {
    //                        P1..P16
    {"plaits", 0x00, {40, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100}},
    {"juno", 0x08, {0, 80, 20, 64, 10, 64, 64, 127}},
    {"daisy", 0x10, {0, 64, 64, 64, 0, 64, 64, 100}},
    {"dsps", 0x20, {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100}},
    // DSP SAMPLE in GRAIN (MOD 56) from the sample RAM: STA 60, END 127 (0.5 s grains), LOP 127 (256 a second):
    // a voice's eight grains always busy, so each retrigger stops sounding grains (after "dsps": Name(0x20))
    {"grain", 0x20, {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 56, 60, 127, 127, 100}},
    {"dd", 0x40, {6, 64, 64, 64, 64, 64, 64, 64}},
    {"fm", 0x48, {0, 64, 64, 64, 64, 64, 100, 127}},
};
const int kN = sizeof kMachs / sizeof kMachs[0];
}  // namespace

CORE_COLD NoteAdapter::NoteAdapter(SysExFn out) : out_(out) {
  for (int v = 0; v < 8; ++v) {
    mach_[v] = 0x00;
    last_[v] = -1;
    Defaults(v);
  }
}

CORE_COLD const char* NoteAdapter::Name(int flag) {
  for (int k = 0; k < kN; ++k)
    if (kMachs[k].flag == flag) return kMachs[k].name;
  return "?";
}

CORE_COLD const char* NoteAdapter::Names() { return "plaits juno daisy dsps grain dd fm"; }

CORE_COLD bool NoteAdapter::SetMachine(int voice, const char* name) {
  if (voice < 0 || voice > 7) return false;
  for (int k = 0; k < kN; ++k)
    if (!strcmp(kMachs[k].name, name)) {
      mach_[voice] = kMachs[k].flag;
      memcpy(p_[voice], kMachs[k].p, 16);   // this entry's (two share DSP SAMPLE's flag)
      return true;
    }
  return false;
}

CORE_COLD void NoteAdapter::Defaults(int voice) {
  for (int k = 0; k < kN; ++k)
    if (kMachs[k].flag == mach_[voice]) memcpy(p_[voice], kMachs[k].p, 16);
}

CORE_COLD void NoteAdapter::Send(int cmd, int voice, int note, int vel) {
  uint8_t m[41] = {0xF0, 0x7D, 0x52, 0x54, (uint8_t)cmd, (uint8_t)(cmd == 0x11 ? voice : (mach_[voice] | voice))};
  int n = 6;
  if (cmd == 0x11) {
    m[n++] = 0xF7;
    out_(m, n);
    return;
  }
  if (cmd == 0x04) {
    m[n++] = (uint8_t)note;
    m[n++] = (uint8_t)vel;
  }
  for (int k = 0; k < 16; ++k) {
    m[n++] = p_[voice][k] & 0x7F;
    m[n++] = 0;
  }
  m[n++] = 0xF7;
  out_(m, n);
}

CORE_COLD void NoteAdapter::Channel(uint8_t status, uint8_t d1, uint8_t d2) {
  int ch = status & 0x0F, type = status & 0xF0;
  if (type == 0xB0 && d1 == 120 && fx::all_sound_off()) log(CORE_STR("sendfx: all sound off -> cleared"));   // any channel
  if (ch > 7) return;
  if (type == 0xB0 && (d1 == 120 || d1 == 123)) {   // All Sound Off: the voice silent; All Notes Off: its note closes
    if (d1 == 120) voice_silence(ch);
    else if (last_[ch] >= 0) Send(0x11, ch, 0, 0);
    last_[ch] = -1;
    return;
  }
  if (type == 0x90 && d2 > 0) {
    last_[ch] = (int8_t)(d1 & 0x7F);
    Send(0x04, ch, d1, d2);
  } else if (type == 0x80 || type == 0x90) {   // off: only the sounding note's closes the voice
    if (last_[ch] != (int8_t)(d1 & 0x7F)) return;
    last_[ch] = -1;
    Send(0x11, ch, 0, 0);
  }
  else if (type == 0xB0 && d1 >= 16 && d1 <= 31) {
    p_[ch][d1 - 16] = d2 & 0x7F;
    Send(0x03, ch, 0, 0);
  }
}

}  // namespace core
