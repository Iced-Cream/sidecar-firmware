// core/device/a4.h - the Analog Four MKII's Overbridge stream, no device profile yet (no mod, no hello). From the firmware (OS 1.55D and 1.56: the
// descriptors, the IN block's builder), Overwitch's device table (GPL-3.0-or-later, facts only) and a unit on a Pi 4 (the id, the product string).
// Alt 3 on interfaces 1 and 2, interrupt IN 0x83 / OUT 0x03, 7 frames a block, as the Rytm; what differs is the count of channels each way.
#pragma once
#include <stdint.h>

namespace core {
namespace device {

constexpr uint16_t kA4Vid = 0x1935, kA4Pid = 0x0b47;   // OVERBRIDGE mode; USB MIDI 1935:000f, USB AUDIO/MIDI 1935:1047
constexpr int kA4InChannels = 8;    // box -> host, 256 B a block: MAIN L R, tracks 1..4, INPUT L R
constexpr int kA4OutChannels = 6;   // host -> box, 200 B a block: MAIN L R, tracks 1..4
constexpr int kA4Main = 0;          // IN's MAIN L; R is the next channel

}  // namespace device
}  // namespace core
