// core/clock/clock.h - the Rytm's step clock as a generic service (2026-11-04, the engine strip-down): the Rytm's step
// ticks (2C) and transport / pattern reports (2D) parsed into plain events, for any listener on the Pi (Pi-side locks,
// and later whatever else needs to know where the Rytm is). It plans nothing and plays nothing: the Sidecar sequencer
// (core/seq) was one listener of these reports and is left in the tree, unwired.
//
// Wire (sim/protocol.py "The Sidecar sequencer", unchanged):
//   2C 01 <blk4> <n> {<track> <step> <len> <speed>} x n    step ticks fired in one 32-frame audio block (blk4: 28 bits)
//   2D 01 01 <state> <blk4>       TRANSPORT: 0 STOP, 1 PLAY, 2 PAUSE
//   2D 01 02 <now> <queued>       PATTERN: the active pattern, the queued one (7F none)
//   2D 01 03 <from> <to> <blk4>   SWITCH: the queued pattern took over in that block
//   2D 01 07 <t hi> <t mid> <t lo> TEMPO: BPM x 120 in 21 bits
//   2D 01 0A <op> <src> ...       EDIT: the stock edit function the Rytm ran (copy / paste / clear / undo ...), raw
//   2D 01 0B ...                  DIRTY, raw
//   other 2D kinds                passed on raw (State::kind, bytes)
// Tracks are 0..kTracks-1: the Rytm's 12, and room for a 13th (the Storage phase's track 12 extension). A tick may carry
// one byte more per tick than the four above in later senders; a tick record's size is taken from the message length.
//
// Threads: sysex() and the listeners on the control side (the Pi's core 3), from host_sysex. Portable.
#pragma once
#include <stdint.h>

namespace core {
namespace clock {

constexpr uint8_t kTickCmd = 0x2C, kStateCmd = 0x2D;
constexpr int kTracks = 13;
constexpr int kBlkFrames = 32;
enum : uint8_t { kTransport = 1, kPattern = 2, kSwitch = 3, kTempo = 7, kEdit = 10, kDirty = 11 };
enum : uint8_t { kStop = 0, kPlay = 1, kPause = 2 };
constexpr uint8_t kNone = 0x7F;

struct Tick {
  uint8_t track, step, len, speed;
  uint8_t extra;       // a fifth byte when the sender adds one per tick (0 otherwise)
  int64_t block;       // the audio block it fired in, unwrapped (blk4 is 28 bits)
};
struct State {
  uint8_t kind;        // 2D's kind byte
  const uint8_t* b;    // the bytes after the kind (valid during the call only)
  int n;
  int64_t block;       // TRANSPORT / SWITCH: the block, unwrapped; -1 for the others
};
using TickFn = void (*)(const Tick&);
using StateFn = void (*)(const State&);
constexpr int kListeners = 4;
bool on_tick(TickFn f);     // false: no room (kListeners each)
bool on_state(StateFn f);

// A whole 2C / 2D (F0 7D 52 54 2C|2D ... F7). True: it was one of them (also when malformed, which is counted)
bool sysex(const uint8_t* d, int len);
void reset();               // the link went down: state unknown again (listeners kept)

// The state as last reported
uint8_t transport();        // kStop / kPlay / kPause
int pattern();              // the active pattern 0..127, -1 unknown
int queued();               // -1 none
double bpm();               // 0 unknown
bool step_of(int track, int* step, int* len, int* speed);   // the track's last tick; false if none since reset

struct Stats { uint32_t ticks, states, bad; };
Stats stats();
}  // namespace clock
}  // namespace core
