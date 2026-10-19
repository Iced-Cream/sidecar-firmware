// core/engine.h - the core's whole API for a platform layer (spec "Layout": the platform calls render(block) and
// feeds the host-link bytes in). In order:
//   1. fill core::hooks (to_rytm at least), then engine_begin() once;
//   2. each whole SysEx from the host: engine_sysex();
//   3. each block: render_voices() (voices.h) - one call for all 8 voices, or disjoint ranges on several cores at
//      once with RYTM_RENDER_CORES set to the core count, rytm_render_core() defined and a mach array per core (voices.h);
//   4. between blocks, while the output has enough queued: idle_prepare(), then idle_save(), then idle_stream(),
//      at most one card job per call (the Teensy puts its own plugin passes between them).
// Single-threaded: steps 1, 2 and 4 never run while a block renders.
#pragma once
#include "host_link.h"
#include "voices.h"

namespace core {

// Boot: the card (its index sorted in Plaits' scratch, before the voices take it), the sample RAM, the JUNO and
// DX7 libraries, the last project's pool and lists (/.rytm/project), the voices. False without a card: the voices
// play, DSP SAMPLE and the lists stay silent / empty.
bool engine_begin();
inline void engine_sysex(const uint8_t* d, int len) { host_sysex(d, len); }
bool idle_prepare();   // a voice's DSP SAMPLE prepare (a waiting trig's first): true if one ran
bool idle_save();      // the project's name to /.rytm/project after a change: true if it wrote
void idle_stream();    // the streams' top-ups (the neediest first) and the sample RAM's loading

}  // namespace core
