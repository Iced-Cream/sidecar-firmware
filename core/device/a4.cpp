// core/device/a4.cpp - see a4.h. Its mod reports as the Rytm's does, on the Sidecar's own head, so the profile lists the
// commands it may send and nothing is translated:
//   03 / 04 / 11   a voice's params, trig, note off (voices 0..3: the synth tracks), as upstream's kA4; the generic engine
//                  has no voices and counts them dropped
//   2C / 2D        the step clock's ticks and transport / pattern / tempo reports (core/clock): tracks 0..5
//   51             STORE: records, lock ops, route ops, graph ops (core/kv/link.h)
// The engine sends it 51 alone (the hello's answer is never filtered): the store's answers and the route's and graphs'
// (60 / 61 / 70 / 71 are ops inside 51). Its records land in /.a4/store/<project>/: the store's "/.rytm/store" path goes
// through state_path as every card path does (core/device/device.h).
#include "device/a4.h"

#include "clock/clock.h"

namespace core {
namespace device {

static_assert(kA4Tracks <= clock::kTracks, "the step clock keeps clock::kTracks tracks");

const Profile kA4 = {
    kIdA4, "Analog Four MKII", kA4Vid, kA4Pid,
    kA4Tracks, 4, 16,
    {0x7D, 0x52, 0x54},
    0x04, 0x03, 0x11,
    0,   // no pages on its screen, kits or patterns from the engine: the generic engine has none
    0, 0, {}, {},
    {0, 0, 1u << (0x51 - 64)},
    nullptr,
    "/.a4",
    {1u << 0x03 | 1u << 0x04 | 1u << 0x11, 1u << (0x2C - 32) | 1u << (0x2D - 32), 1u << (0x51 - 64)},
    kBracketFull, nullptr, nullptr};

}  // namespace device
}  // namespace core
