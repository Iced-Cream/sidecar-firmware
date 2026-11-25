// The measured FX guard's audio-buffer view (cores.cpp RenderBlock; docs/fx-measured-mute.md rule 4). Header only: its
// test builds it as it is.
//
// A base pass begun with the output FIFO under a block makes the render core that took longer in the last block late
// (decided 2026-11-25 after LT12g P5). Held for kHold base passes after each dip (LT12h P5, 2026-11-25: the control
// loop renders up to a FIFO target of blocks a pass, so only a pass's first block began low and the next ones began with
// it refilled; late and on-time blocks alternated, the late rule's 4 in a row never came, and nothing was muted while
// the FIFO drained). Not for kGrace passes after a stream (re)start, whose FIFO begins empty.
#pragma once
#include <stdint.h>

namespace fifo_late {

struct State {
  static constexpr unsigned kGrace = 64;   // base passes after a stream (re)start
  static constexpr unsigned kHold = 32;    // base passes late after a dip (8 ms: the late rule's 4, its 32-block settle)
  unsigned grace = kGrace, left = 0, streams = 0;
  // one base pass: true when it is late. dip: set when the FIFO was under a block at this pass (the dips counted)
  bool step(uint32_t level, unsigned streams_now, uint32_t block, bool on, bool* dip = nullptr) {
    if (dip) *dip = false;
    if (streams_now != streams) streams = streams_now, grace = kGrace, left = 0;
    if (grace) {
      --grace;
      return false;
    }
    if (!on) return left = 0, false;
    if (level < block) {
      left = kHold;
      if (dip) *dip = true;
    }
    if (!left) return false;
    --left;
    return true;
  }
};

}  // namespace fifo_late
