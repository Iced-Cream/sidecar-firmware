// The measured FX guard's audio-buffer view (cores.cpp RenderBlock; docs/fx-measured-mute.md rule 4). Header only: its
// test builds it as it is.
//
// The FIFO is low under kLowBlocks blocks at a base pass's start. Each low run's first block is a dip (one control pass
// that came late or one render pass that fell short). Two dips within kHold base passes make the render core that took
// longer in the last block late, and keep it late for kHold passes from the second dip.
//   2026-11-25 LT12g P5: no core past a block, yet the FIFO drained 14 s: the FIFO's view decided (Connor).
//   LT12h P5: the control loop refills the FIFO over a pass, so only a pass's first block began low; late and on-time
//   blocks alternated and the late rule's 4 in a row never came: hence the hold.
//   LT12i P5: at 1 block the mute landed only once the FIFO was empty (138 underruns, a scrape): 2 blocks (Connor).
//   Two dips, not one: a single long control pass (pass max reaches ~3 ms in normal play) dips the FIFO once and refills
//   it; an overload dips it at every pass. Not for kGrace passes after a stream (re)start, whose FIFO begins empty.
#pragma once
#include <stdint.h>

namespace fifo_late {

struct State {
  static constexpr unsigned kGrace = 64;     // base passes after a stream (re)start
  static constexpr unsigned kLowBlocks = 2;  // under this many blocks in the FIFO: low
  static constexpr unsigned kHold = 32;      // base passes: the window for the second dip, and late after it (8 ms)
  unsigned grace = kGrace, left = 0, streams = 0, since = kHold;   // since: base passes since the last dip
  bool low = false;                                                // the last base pass began low
  // one base pass: true when it is late. dip: set when this pass began a low run (the dips counted)
  bool step(uint32_t level, unsigned streams_now, uint32_t block, bool on, bool* dip = nullptr) {
    if (dip) *dip = false;
    if (streams_now != streams) streams = streams_now, grace = kGrace, left = 0, since = kHold, low = false;
    if (grace) {
      --grace;
      return false;
    }
    if (!on) return left = 0, low = false, false;
    const bool now_low = level < kLowBlocks * block;
    if (now_low && !low) {   // a dip: a second within kHold passes arms the hold
      if (since < kHold) left = kHold;
      since = 0;
      if (dip) *dip = true;
    } else if (since < kHold) {
      ++since;
    }
    low = now_low;
    if (!left) return false;
    --left;
    return true;
  }
  // the hold still on, without a pass of its own (the other FIFO's passes: cores.cpp)
  bool held() const { return grace == 0 && left > 0; }
};

}  // namespace fifo_late
