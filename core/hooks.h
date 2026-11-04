// core/hooks.h - the platform layer's entry points into the core: where the core's SysEx for the host goes, and the
// restart another box's state folder needs.
#pragma once
#include <stdint.h>

namespace core {

struct Hooks {
  void (*to_rytm)(const uint8_t* m, int n);   // a SysEx F0..F7 for the host; null: dropped
  // Another box needs another state folder (core/device/device.h): saved, restart the Sidecar. Null: a board that cannot.
  void (*device_restart)();
};
extern Hooks hooks;

uint32_t to_rytm_dropped();                  // messages to_rytm dropped (no hook)
void to_rytm(const uint8_t* m, int n);

}  // namespace core
