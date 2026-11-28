// core/device/a4.h - the Analog Four MKII: its Overbridge stream, and the profile of an A4 running the sidecar-mods a4 mod (not official support,
// docs/device-profiles.md). From the firmware (OS 1.55D and 1.56: the descriptors, the IN block's builder, 0x400febf0 the OUT words), Overwitch's device
// table (GPL-3.0-or-later, facts only) and a unit on a Pi 4 (the id, the product string). Alt 3 on interfaces 1 and 2, interrupt IN 0x83 / OUT 0x03, 7 frames
// a block, as the Rytm; what differs is the count of channels each way. Four synth tracks as four Sidecar voices, each back on its own channel. Checked in an
// emulator and in host tests only, never on an A4.
#pragma once
#include <stdint.h>

#include "device/device.h"

namespace core {
namespace device {

constexpr uint16_t kA4Vid = 0x1935, kA4Pid = 0x0b47;   // OVERBRIDGE mode; USB MIDI 1935:000f, USB AUDIO/MIDI 1935:1047
constexpr int kA4InChannels = 8;    // box -> host, 256 B a block: MAIN L R, tracks 1..4, INPUT L R
constexpr int kA4OutChannels = 6;   // host -> box, 200 B a block: MAIN L R, tracks 1..4
constexpr int kA4Main = 0;          // IN's MAIN L; R is the next channel
constexpr uint16_t kIdA4 = 3;
extern const Profile kA4;           // kBracketFull: the voices through the A4's own filters, amp and FX

}  // namespace device
}  // namespace core
