// core/device/audio.h - a box's Overbridge stream, as a table (the device profiles' audio row, core/device/device.h; the
// Pi's Overbridge driver, pi/engine/ob_usb.cpp): moved here from the full engine's core/fx/out.h (the voices' OUT route,
// cut in the strip-down of 2026-11-04), which kept the table beside the routes.
#pragma once
#include <stdint.h>

namespace core {
namespace out {

constexpr int kVoices = 8;
// The device the voices go into, as a table (device-agnostic host side: a future device - an Analog Four MKII - adds
// its own row and its firmware mod, nothing else here changes): its host -> device channels for the main return
// (`post`, after its master FX), the FX return (`pre`, before them), each voice's own channel (`voice`), the index of
// the voice's stem in the device -> host stems (`stem`: the send FX's input the voice stands in for on PRE / POST),
// the tracks that play each voice (a shared voice: two; -1 none) for the mute gate, the device -> host channels of its
// AUDIO IN (`audio_in`: what LIVE IN plays, core/livein/livein.h) and its own OUT -> IN loop (`loop_frames`: host -> device
// frames back in the device -> host stream, the converters and the device's buffers; LIVE IN's latency estimate). The only
// device so far: the Analog Rytm MKII over Overbridge (device-specific: docs/STEREO.md "Device-specific").
// `ret` is how the voices go back: per voice on their own channels, or a stereo mix on `post` (every voice POST, whatever its
// OUT says: LEV, PAN and WID still apply; a box with two return channels, docs/device-profiles.md). A channel a device does
// not have is kNoChannel. in_channels: device -> host channels a frame (the driver's consumers take 12).
constexpr uint8_t kNoChannel = 0xFF;
enum Return : uint8_t { kPerVoice = 0, kStereoMix = 1 };
struct Device {
  const char* name;
  uint8_t channels;          // host -> device channels a frame
  uint8_t post[2], pre[2];   // the returns' L / R channels
  uint8_t voice[kVoices];    // each voice's own channel (TRK)
  uint8_t stem[kVoices];     // each voice's stem (0.. in the device's stem order)
  int8_t tracks[kVoices][2];
  uint8_t audio_in[2];       // device -> host: AUDIO IN L / R
  uint16_t loop_frames;      // host -> device -> host, frames (0: not measured)
  uint8_t in_channels;       // device -> host channels a frame
  Return ret;                // per voice, or one stereo mix
};
// the Rytm: voices BD BT SD LT CH/OH MT/HT CY/CB RS/CP (core/ui/remote.cpp kTrackVoice), channels 4..11 / stems
// IN 2..9 in the order BD SD RS/CP BT LT MT/HT CH/OH CY/CB, tracks BD SD RS CP BT LT MT HT CH OH CY CB; AUDIO IN L / R on
// IN 10 / 11 (ob_block.h); the loop 289 frames [measured 2026-10-15 on the Rytm, docs/pi-engine.md: a voice's onset as
// sent in OUT -> back in MAIN on IN, FIFO not included]
inline constexpr Device kRytmMk2 = {"Analog Rytm MKII", 12, {0, 1}, {2, 3}, {4, 7, 5, 8, 10, 9, 11, 6}, {0, 3, 1, 4, 6, 5, 7, 2},
                                    {{0, -1}, {4, -1}, {1, -1}, {5, -1}, {8, 9}, {6, 7}, {10, 11}, {2, 3}}, {10, 11}, 289, 12, kPerVoice};

}  // namespace out
}  // namespace core
