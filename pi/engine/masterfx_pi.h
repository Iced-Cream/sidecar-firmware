// pi/engine/masterfx_pi.h - the master FX (core/fx/master.h, docs/MASTER-FX.md) in the Pi engine. The chain runs on render
// core 2, after the send FX. Process, in the OUT fill on core 0, is the one place that touches the channels: it sends
// what is on 0/1 and 2/3 to core 2 and adds what comes back on 0/1, crossfading between that and the direct path. Nothing
// is written to the channels while no chain is set, or while it is bypassed and faded: the output is then bit for bit
// what it was without master FX (take playback comes after it, so a take or a looper is not processed twice).
//
// The latency the chain adds on POST is the ring back to core 0, kPrime frames (2 ms: the send FX's margin against core
// 3's longest pass) plus up to one URB's frames; the chain's own effects add none. The direct path adds none. Bypass keeps
// the chain running (tails go on, the core time stays, re-engaging is instant). Hosted (no Circle headers): ob_usb.cpp and
// cores.cpp call it.
#pragma once
#include <stdint.h>

namespace masterfx_pi {
const unsigned kRing = 1024, kPrime = 96, kMaxPerRender = 96, kFadeFrames = 480;   // frames; the crossfade 10 ms
const unsigned kStemPrime = 64;                                                    // SRC MIX: the stems' queue, frames
// The returns' gains into the mix differ: measured on the real Rytm (2026-11-01, stereo=measure, master FX neutral: POST
// -10.9 dB, PRE -2.0 dB), and the stems reach MAIN about at unity [inferred]. So what is moved or added onto 0/1 is raised
// to keep its level in the mix: 2/3 by +8.9 dB, the stems by +10.9 dB. What was on 0/1 stays at unity. The Rytm's own pan
// law keeps L + R constant and linear (sidecar-mods re/subsystems/compressor-hardware.md), so a centred stem has 1 on each side and a hard one 2 on one
// side; stems 5 and 6 (MT/HT, CH/OH) read 0.85 dB hot against what they put in MAIN, and are trimmed.
constexpr float kPreOnPost = 2.786f, kStemOnPost = 3.508f, kStemHotTrim = 0.9067f;
const unsigned kGainFrames = 512;   // a stem's pan or its coming off main: at most this long, end to end
// core 0, the OUT fill, after AddReturn and before take playback: out[n] as about to be sent
void Process(int32_t (*out)[12], unsigned n);
// SRC MIX: core 0, the IN completion: the Rytm's stems IN 2..9 while it is asked for (WantStems): each stem panned as the
// Rytm reports it (master::report), none that is still on main; no report: all of them, centred
bool WantStems();
void Stems(const int32_t (*stems)[8], unsigned n);
void Render();          // core 2, after the send FX: the frames queued
bool Command(const char* p);   // core 3: mfx=stats (logged): since the last one, core 2's load, underruns, drops
// The stats, for the display (any core, plain reads)
struct Stats { uint32_t frames, under, drop_in, drop_out, stem_under; };
Stats Read();
float Wet();            // the crossfade's position, 0 direct .. 1 the chain's output (core 0 writes)
void Reset();           // a new stream (core 0): the rings' frames forgotten; tests
}  // namespace masterfx_pi
