// core/voice_name.h - a Sidecar voice is named for the Rytm track(s) that play it when it is shown to the user, never a
// number (voice 1 is BT's, voice 2 SD's; the routing is core/ui/remote.cpp kTrackVoice). The console, the wire and the
// card files keep "V1".."V8".
#pragma once

#include "device/device.h"

namespace core {

inline const char* voice_name(int v) {
  static const char* const k[8] = {"BD", "BT", "SD", "LT", "CH/OH", "MT/HT", "CY/CB", "RS/CP"};
  static const char* const pair[4] = {"CP", "HT", "OH", "CB"};   // a slot's voices 8..11: the Rytm's track reports
  if (in_slot(v) >= kBoxVoices && in_slot(v) < 12 && !device::box_screen()) return pair[in_slot(v) - kBoxVoices];
  if (const device::Screen* sc = device::box_screen()) return sc->names[box_voice(v)];   // (the box read once: rc040 review L7)
  return k[box_voice(v)];
}

}  // namespace core
