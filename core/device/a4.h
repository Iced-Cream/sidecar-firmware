// core/device/a4.h - the Analog Four MKII with an A4 Sidecar mod that reports as the Rytm's companion does: not official
// support (docs/device-profiles.md). The generic engine's services only: the host link's 4F hello, 51 STORE (records, locks,
// routes, graphs) and the step clock's 2C / 2D, with its state in /.a4. Six tracks: synth 1..4, FX, CV. Checked in host tests
// only, never on an A4.
//
// No audio row on this line: the Pi's Overbridge driver takes 12 device -> host channels and the A4 sends 8 (MAIN L R, the four
// voices, INPUT L R), so a row here would only be refused (pi/engine/ob_usb.cpp). The A4's stream, its 8 -> 12 widening and the
// stock-A4 mirror are on the full engine's line (upstream dev, kA4Audio there); the row joins this profile when the driver
// takes a box with another channel count.
#pragma once
#include <stdint.h>

#include "device/device.h"

namespace core {
namespace device {

constexpr uint16_t kA4Vid = 0x1935, kA4Pid = 0x0b47;   // OVERBRIDGE mode; USB MIDI 1935:000f, USB AUDIO/MIDI 1935:1047
constexpr uint16_t kIdA4 = 3;                          // the id upstream's kA4 has (core/device/a4.h on dev)
constexpr int kA4Tracks = 6;                           // synth 1..4, FX 4, CV 5: the step clock's tracks 0..5
extern const Profile kA4;                              // kBracketFull: the voices through the A4's own filters, amp and FX

}  // namespace device
}  // namespace core
