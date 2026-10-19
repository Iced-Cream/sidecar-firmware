// core/slots.h - the user slots U1..U5 on a board without machine plugins (moved from voiceloop.ino's none_*):
// every slot EMPTY at generation 1. A 19 for its metadata gets 1A part 0 with state 0 (43 bytes), or "unchanged"
// (10) when it asked at generation 1; a 19 what 3 gets an empty installed list (1B 0x21), so the slot menu's YES
// sends a 1E clear, which gets NOT INSTALLED (any 1E does); a 10 and a 19 what 3 get the 13-byte 1D, at most once
// a second (the Rytm's us.seen set, no endless 1A re-asks), and the platform's loop calls slots_1d(5000) anyway.
#pragma once
#include <stdint.h>

namespace core {
void slots_ask(const uint8_t* d, int len);      // 0x19
void slots_assign(const uint8_t* d, int len);   // 0x1E
bool slots_1d(uint32_t gap_ms);                 // the 13-byte 1D unless the last went out under gap_ms ago
}  // namespace core
