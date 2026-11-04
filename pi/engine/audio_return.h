// pi/engine/audio_return.h - the audio path back to the Rytm in the stripped engine (2026-11-04): what the Sidecar
// sends on Overbridge OUT. The engine renders nothing of its own: OUT carries the computer's audio from the USB-C
// Sidecar (sidecar_gadget.h, as before) and what core/proc's processor slots add. With every slot empty (the stripped
// engine's default: no processor is built in) the fill below adds nothing and costs one test a block, so OUT is the
// pass-through it was.
//
// Where the processors run, for now: core 0, in the OUT fill (the xHCI interrupt or the paced pump), on the IN frames
// the IN completion handed over. A processor there has the fill's time only; moving the slots onto a render core (a
// ring each way, as the master FX had) comes with the first processor that needs more. Hosted (no Circle headers).
#pragma once
#include <stdint.h>

namespace aret {
const int kChannels = 12;
const int kMaxFrames = 64;
bool Wanted();                                       // a processor slot is filled: In() keeps the IN frames
void Reset();                                        // a new stream: the IN frames left from the last one dropped
void In(const int32_t (*in)[kChannels], int n);      // core 0, the IN completion: the Rytm's frames (only kept while a slot runs)
void Fill(int32_t (*out)[kChannels], int n);         // core 0, the OUT fill: the slots' output added to out (n <= kMaxFrames)
uint32_t InDropped();                                // IN frames that found the ring full (the OUT side fell behind)
uint32_t InMissing();                                // OUT frames whose IN frame had not come (zeros used)
}  // namespace aret
