// core/route/route.h - the Sidecar's generic audio route (the audio route phase, decided 2026-11-04: "ROUTE op +
// lockable gains"). A Rytm mod asks for a patch point: one of the Rytm's Overbridge IN channels (a voice stem or AUDIO
// IN) added onto one Overbridge OUT channel (MAIN, MAIN FX or a voice input) at a gain. The Pi hands out the point's
// number and remembers which mod owns it; the gain is also a parameter the lock service reaches (lock target kTarget,
// param = the point). Nothing here knows what a mod means by a route, and nothing changes the sound but the gain.
//
// Channels (Overbridge, 0-based):
//   IN  (Rytm -> Pi)  0-1 MAIN L/R, 2-9 the voice stems BD SD RS/CP BT LT MT/HT CH/OH CY/CB, 10-11 AUDIO IN L/R
//   OUT (Pi -> Rytm)  0-1 MAIN L/R, 2-3 MAIN FX L/R, 4-11 the voice inputs in the same voice order
// Processor slots (core/proc; graphs, the send FX phase 2026-11-05): 12 + 2 s + k is slot s's channel k (s 0..15,
//   k 0 L / 1 R): as a destination, the slot's input (what the processor reads, b.sin); as a source, its output
//   (b.sout). A slot's output may feed OUT or another slot's input (a chain): a graph of library blocks.
// Order in one block: the points from IN channels (process_in), then each slot in graph order (order(): a slot runs
// after every slot that feeds it), each followed by the points from its outputs (process_slot), then end_block.
// Refused (61 REFUSED codes, sim/protocol.py): MAIN as a source (it already holds what the Pi returns), and a point
// that closes a loop: a stem back into its own input, or a cycle through other voices' inputs or processor slots (a
// slot counts as one node from its input to its output: SD stem -> slot 0, slot 0 -> SD input is a loop; slot 0 ->
// slot 1 -> slot 0 too). Feedback lives inside a block (core/dsp), never in the graph.
//
// Host link (51 STORE, version 1; the control core's, as the lock ops):
//   Rytm -> Pi  20 ROUTE   <seq> <owner> <src 2..43> <dst 0..43> <gain hi> <lo>   a new point; answer 60 or 61
//               21 MOVE    <seq> <owner> <point> <src> <dst>                      re-aim an owned point; answer 60 or 61
//               22 UNROUTE <owner> <point | 7F every point of owner>              no answer
//               23 GAIN    <owner> <point> <hi> <lo>                              no answer (a lock does the same)
//               24 MOD     <seq> <owner> <src slot> <ctl> <dst slot> <param> <depth hi> <lo>   a mod point; 60 / 61
//   Mod points (2026-11-10, Connor's pick for the voice graphs): slot src's control output ctl (0..3: an envelope, an
//   LFO; core/proc ctl()) onto slot dst's param, at a depth (8192 none; core/proc set_mod). Their point numbers are
//   kPoints.. (64..95): UNROUTE closes one (7F: every point and mod point of the owner), GAIN sets its depth, FREE
//   of either slot and HELLO close it. A mod point is an edge for the loop check and the graph order, as a point
//   from src's output to dst's input is: refused (7) when dst feeds src, or src = dst.
//   Pi -> Rytm  60 ROUTED  <seq> <owner> <point>     61 REFUSED <seq> <owner> <code> (3 full, 5 bad, 7 loop)
//               (not 7F STATUS: the store's requests keep their own seq; owner + seq name the asker)
// Gains: 14 bits, 8192 = unity (0 dB), 16383 = +6 dB, 0 silent; a change ramps over kRamp frames (5 ms) so a lock on a
// step does not click. A source / destination change fades the point out, re-aims it, and fades it back in.
// Stamped gains (2026-11-05, after T118 [hw]: a lock applied when its tick arrived let the first 1-2 ms of the step's
// hit through): the lock sink calls stamp(tick's block) before a tick's values, and those gains land by a frame, not at
// once. The Rytm's block counter (32 frames a block) maps to the render side's frame count by the earliest a tick has
// arrived (frames rendered - 32 x block, the least over this and the last 256 ticks: MIDI's jitter taken out), plus a
// lead (`locklead=` frames, kLead by default): the frames the step's audio takes, past the earliest tick, to reach the
// render side. A gain due later than a ramp from now waits and ramps to land on its frame; one due sooner ramps over
// what is left (at least kMinRamp frames: counted as late).
// Lifecycle: the companion's HELLO (and a link reset) closes every point; the Rytm asks again once answered.
//
// Threads: sysex / set_gain / reset / take_answer on the control core (one producer); process() on the render side
// (core 0's OUT fill, one consumer). With no point open and nothing fading, process() returns at once and OUT stays the
// pass-through it was. Portable (Pi, Mac core, Linux tests).
#pragma once
#include <stdint.h>

namespace core {
namespace route {

constexpr int kPoints = 64;       // 16 until the send FX phase (2026-11-05): sends, chains and returns of two graphs
constexpr int kTarget = 64;       // the lock target of the gains (4 until 2026-11-05, when 4 became a processor slot)
constexpr int kChannels = 12;     // Overbridge IN / OUT
constexpr int kSlots = 16;        // processor slots (core/proc kSlots)
constexpr int kSlotChans = 2 * kSlots;   // their channels (L, R)
constexpr int kNodes = kChannels + kSlotChans;   // a point's src / dst numbers: 0..19
constexpr int slot_chan(int slot, int k) { return kChannels + 2 * slot + k; }
constexpr int kRamp = 240;        // frames a gain change takes (5 ms at 48 kHz)
constexpr int kUnity = 8192;
constexpr int kBlockFrames = 32;  // the Rytm's audio block (core/clock's blk4 counts these)
constexpr int kLead = 144;        // locklead= default: 3 ms (T118 [hw]: ticks reached the Pi ~4 ms before the hit)
constexpr int kMinRamp = 48;      // 1 ms
enum Op : uint8_t { kRoute = 0x20, kMove = 0x21, kUnroute = 0x22, kGain = 0x23, kMod = 0x24, kRouted = 0x60, kRefused = 0x61 };
constexpr int kMods = 32;         // mod points (core/proc kMods); numbered kPoints..kPoints+kMods-1
enum Code : uint8_t { kFull = 3, kBad = 5, kLoop = 7 };

bool is_route_op(uint8_t op);
// a whole 51 message with a route op; false: malformed or refused (an answer is queued for ROUTE / MOVE either way)
bool sysex(const uint8_t* m, int n);
// the next queued answer (60 / 61), whole F0..F7; 0: none. Control core.
int take_answer(uint8_t* out, int cap);
bool set_gain(int point, int value14);   // the lock sink's (target kTarget); false: no such open point / range
void stamp(int64_t block);               // the next set_gain()s land by this Rytm block's audio; -1: at once again
void set_lead(int frames);               // locklead= (0..4800)
int64_t frames();                        // frames process() has rendered (the stamp's time base)
void reset();                            // every point closed (fades out)
bool any();                              // a point is open or still fading: the IN frames are needed

// Render side, once a block, in this order:
//   process_in    in[c] the Rytm's IN channel c, out[c] OUT channel c, sin[2 s + k] slot s's input channel k: the points
//                 from IN channels added onto OUT and the slots' inputs; drains the control side's changes
//   order         the 16 slots in graph order into slots[] (a slot after every slot that feeds it; a cycle left by a
//                 fade, never one the control side allowed, falls back to slot order: one block late for that edge)
//   for each slot s in that order: the slot's processor, then process_slot(s): the points from s's outputs
//                 (sout[2 s + k]) added onto OUT and other slots' inputs
//   end_block     the frame count moves on
// sin / sout null: points to / from slots are skipped.
void process_in(const float* const* in, float* const* out, float* const* sin, int frames);
void order(uint8_t* slots);
void process_slot(int s, const float* const* sout, float* const* out, float* const* sin, int frames);
void end_block(int frames);
// all of it with no slot channels (a build without processors, the tests)
void process(const float* const* in, float* const* out, int frames);
// control side: every open point to or from slot s closes (fades out), whoever owns it (core/dsp's FREE)
void close_slot(int s);
// control side: no point to or from slot s is open or still fading (its processor can go without a click)
bool slot_quiet(int s);

// tests / /status / console
struct Point { bool open; uint8_t owner, src, dst; uint16_t gain; };
Point point(int p);   // the control side's view
struct ModPoint { bool open; uint8_t owner, src, ctl, dst, param; uint16_t depth; };
ModPoint mod_point(int i);   // i 0..kMods-1 (point number kPoints + i)
struct Stats {
  uint32_t routed, moved, closed, refused, gains, ops_bad; int open; int last_refused;
  uint32_t timed, late, resyncs;   // stamped gains; those too late for a whole ramp; stamp estimates started again
  int lead, spread;                // locklead; the ticks' arrival jitter (frames, latest window)
};
Stats stats();
// "SRC>DST@GAIN" names for the console: "SD>MAIN L", "SD>S0 L", "S0 L>FX L" etc. (c 0..19)
const char* in_name(int c);
const char* out_name(int c);

}  // namespace route
}  // namespace core
