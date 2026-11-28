// core/clock/clock.h - a box's step clock as a generic service (2026-11-04, the engine strip-down; one per slot since S8a,
// 2026-11-28): the box's step ticks (2C) and transport / pattern reports (2D) parsed into plain events, for any listener on
// the Pi (Pi-side locks, and later whatever else needs to know where a box is). It plans nothing and plays nothing: the
// Sidecar sequencer (core/seq) was one listener of these reports and is left in the tree, unwired.
//
// One Clock per slot (kSlots, the two boxes of a two-box Pi), each fed by its own box's 2C / 2D and nothing else: two boxes
// run on their own clocks (decided 2026-11-28), so nothing here is shared between slots - state, block unwrap, stats and
// listeners are each slot's own. slot(i) is slot i's; the free functions below are slot 0's, as the one-box engine has it.
//
// Wire (sim/protocol.py "The Sidecar sequencer", unchanged):
//   2C 01 <blk4> <n> {<track> <step> <len> <speed>} x n    step ticks fired in one 32-frame audio block (blk4: 28 bits)
//   2D 01 01 <state> <blk4>       TRANSPORT: 0 STOP, 1 PLAY, 2 PAUSE
//   2D 01 02 <now> <queued>       PATTERN: the active pattern, the queued one (7F none)
//   2D 01 03 <from> <to> <blk4>   SWITCH: the queued pattern took over in that block
//   2D 01 07 <t hi> <t mid> <t lo> TEMPO: BPM x 120 in 21 bits
//   2D 01 0A <op> <src> ...       EDIT: the stock edit function the box ran (copy / paste / clear / undo ...), raw
//   2D 01 0B ...                  DIRTY, raw
//   other 2D kinds                passed on raw (State::kind, bytes)
// Tracks are 0..tracks()-1, the slot's box's: kTracks (13) for the Rytm (its 12 and room for the Storage phase's track 12
// extension), its profile's count for another box (tracks_for), so a tick on a track the box does not have reaches no
// listener and is counted off_track. A tick may carry one byte more per tick than the four above in later senders; a tick
// record's size is taken from the message length.
//
// Threads: sysex() and the listeners on the control side (the Pi's core 3), from host_sysex. Portable.
#pragma once
#include <stdint.h>

namespace core {
namespace device { struct Profile; }
namespace clock {

constexpr uint8_t kTickCmd = 0x2C, kStateCmd = 0x2D;
constexpr int kTracks = 13;
constexpr int kBlkFrames = 32;
constexpr int kSlots = 2;
enum : uint8_t { kTransport = 1, kPattern = 2, kSwitch = 3, kTempo = 7, kEdit = 10, kDirty = 11 };
enum : uint8_t { kStop = 0, kPlay = 1, kPause = 2 };
constexpr uint8_t kNone = 0x7F;

struct Tick {
  uint8_t track, step, len, speed;
  uint8_t extra;       // a fifth byte when the sender adds one per tick (0 otherwise)
  uint8_t slot;        // the slot whose box sent it
  int64_t block;       // the audio block it fired in, unwrapped (blk4 is 28 bits), on that box's own clock
};
struct State {
  uint8_t kind;        // 2D's kind byte
  const uint8_t* b;    // the bytes after the kind (valid during the call only)
  int n;
  int64_t block;       // TRANSPORT / SWITCH: the block, unwrapped; -1 for the others
  uint8_t slot;        // the slot whose box sent it
};
using TickFn = void (*)(const Tick&);
using StateFn = void (*)(const State&);
constexpr int kListeners = 4;

struct Stats { uint32_t ticks, states, bad, off_track; };   // off_track: ticks on a track >= tracks()

// The tracks a box has for its slot's clock: kTracks for the Rytm (null or kIdRytm), else its profile's, at most kTracks
int tracks_for(const device::Profile* p);

class Clock {
 public:
  bool on_tick(TickFn f);     // false: no room (kListeners each); this slot's ticks only
  bool on_state(StateFn f);

  // A whole 2C / 2D (F0 7D 52 54 2C|2D ... F7) from this slot's box. True: it was one of them (also when malformed, which
  // is counted)
  bool sysex(const uint8_t* d, int len);
  void reset();               // the slot's link went down: state unknown again (listeners and tracks kept)

  // The slot's box: its track count (tracks_for). Kept over reset(); whoever binds a box to the slot sets it.
  void set_tracks(int n);
  int tracks() const { return n_tracks_; }

  // The state as last reported
  uint8_t transport() const { return transport_; }   // kStop / kPlay / kPause
  int pattern() const { return pattern_; }           // the active pattern 0..127, -1 unknown
  int queued() const { return queued_; }             // -1 none
  double bpm() const { return bpm_; }                // 0 unknown
  bool step_of(int track, int* step, int* len, int* speed) const;   // the track's last tick; false if none since reset
  Stats stats() const { return st_; }
  int slot() const;

 private:
  int64_t unwrap(const uint8_t* b);
  void ticks(const uint8_t* b, int n);
  void state(const uint8_t* a, int n);

  struct Last { bool on; uint8_t step, len, speed; };
  TickFn tick_fn_[kListeners] = {};
  StateFn state_fn_[kListeners] = {};
  uint8_t transport_ = kStop;
  int pattern_ = -1, queued_ = -1;
  double bpm_ = 0;
  Last last_[kTracks] = {};
  Stats st_ = {};
  int n_tracks_ = kTracks;
  bool have_blk_ = false;
  uint32_t last_blk_ = 0;
  int64_t blk_hi_ = 0;
};

Clock& slot(int s);           // slot s's clock (0..kSlots-1; anything else is slot 0's)

// Slot 0's, as the one-box engine calls them
bool on_tick(TickFn f);
bool on_state(StateFn f);
bool sysex(const uint8_t* d, int len);
void reset();
uint8_t transport();
int pattern();
int queued();
double bpm();
bool step_of(int track, int* step, int* len, int* speed);
int tracks();
Stats stats();
}  // namespace clock
}  // namespace core
