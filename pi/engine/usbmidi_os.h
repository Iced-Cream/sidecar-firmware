// pi/engine/usbmidi_os.h - the USB-C MIDI port's OS-upgrade setting, worded as the Rytm MIDI bridge's (midibridge.h:
// midibridge=os:on|off). Portable: tests/test_usbmidi_os.cpp.
//
//   usbmidi=os:off   (default) Elektron OS upgrades from the computer are dropped, as in stock Sidecar
//   usbmidi=os:emu   they pass only while the TCP link (rytm-emu) is the host
//   usbmidi=os:on    they pass to the real Rytm on USB too (and to the emulator under link=tcp)
// The same words work in cmdline.txt and as a serial console command (POST /cmd refuses them, web.cpp, as midibridge=). The older osupdate=off|0|emu|1|rytm is kept
// as an alias (osupdate=1 is emu, osupdate=rytm is os:on); a valid usbmidi= wins when cmdline.txt has both.
#pragma once
#include <string.h>

namespace usbmidi_os {

enum Mode { kOff, kEmu, kOn, kInvalid };

// the value after "usbmidi=": os:on|os:off|os:emu
inline Mode parse_usbmidi(const char* v) {
  if (v == nullptr || strncmp(v, "os:", 3) != 0) return kInvalid;
  v += 3;
  if (strcmp(v, "off") == 0) return kOff;
  if (strcmp(v, "emu") == 0) return kEmu;
  if (strcmp(v, "on") == 0) return kOn;
  return kInvalid;
}

// the value after "osupdate=" (the alias): off|0, emu|1, rytm
inline Mode parse_osupdate(const char* v) {
  if (v == nullptr) return kInvalid;
  if (strcmp(v, "off") == 0 || strcmp(v, "0") == 0) return kOff;
  if (strcmp(v, "emu") == 0 || strcmp(v, "1") == 0) return kEmu;
  if (strcmp(v, "rytm") == 0) return kOn;
  return kInvalid;
}

// a whole command or cmdline word ("usbmidi=os:on", "osupdate=rytm"); kInvalid when it is neither key or a bad value.
// *known: the key was one of the two (so a bad value is reported, not passed on to other commands)
inline Mode parse_command(const char* p, bool* known) {
  *known = true;
  if (strncmp(p, "usbmidi=", 8) == 0) return parse_usbmidi(p + 8);
  if (strncmp(p, "osupdate=", 9) == 0) return parse_osupdate(p + 9);
  *known = false;
  return kInvalid;
}

// cmdline.txt: usbmidi= first, then the alias, else off
inline Mode from_cmdline(const char* usbmidi, const char* osupdate) {
  Mode m = usbmidi != nullptr ? parse_usbmidi(usbmidi) : kInvalid;
  if (m == kInvalid && osupdate != nullptr) m = parse_osupdate(osupdate);
  return m == kInvalid ? kOff : m;
}

inline const char* name(Mode m) { return m == kOn ? "on" : m == kEmu ? "emu" : "off"; }

}  // namespace usbmidi_os
