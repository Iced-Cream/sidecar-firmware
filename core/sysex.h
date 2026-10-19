// core/sysex.h - the SysEx messages the engine sends the host, F0 7D 52 54 <cmd> ... F7 (sim/protocol.py has
// their Python twins; rytm-mods 0021/0023 read them). Each builder writes into m and returns the length.
#pragma once
#include <stdint.h>

namespace core {

// 13 name bytes: byte i < len of name kept if printable ASCII (else '_'), a NUL stays NUL, NUL padded to 13.
void name13(uint8_t* out, const char* name, int len);

// 14 <replace> <count> { <slot> <folder hi lo> <file hi lo> <13 name bytes> }, count <= 6 (<= 116 bytes).
// folder/file 0x3fff: not on the card. name/len as name13 (the caller cuts the extension).
struct PoolRow {
  uint8_t slot;
  uint16_t folder, file;
  const char* name;
  int len;
};
int sysex_pool(uint8_t* m, bool replace, const PoolRow* rows, int count);

// 16 (JUNO) / 17 (FM) <replace> <total> { <slot> <13 name bytes> }, count <= 8 (<= 120 bytes): slots
// first..first+count-1; names[k] null or "" is an empty slot (13 NULs).
int sysex_names(uint8_t* m, uint8_t cmd, bool replace, int total, int first, const char* const* names, int count);

// 15 <voice> <CUT RES ENV ATK DEC REL> (13 bytes): each value & 0x7f.
int sysex_juno_knobs(uint8_t* m, int voice, const uint8_t knobs[6]);

// 0E <folder hi lo> <parent hi lo> <first hi lo> <total hi lo> { <kind> <id hi lo> <size 4 x 7 bits> <len> <name> },
// count <= 6 (m: 256 bytes). size clipped to 2^28 - 1; name printable, at most 23 characters.
struct ListEntry {
  uint8_t kind;   // 1 a folder, 0 a file / patch / voice
  uint32_t id, size;
  const char* name;
};
int sysex_listing(uint8_t* m, int folder, int parent, int first, int total, const ListEntry* e, int count);

// The user slots without machine plugins (core/slots.cpp): 1A part 0 EMPTY at generation 1 (43 bytes), 1A
// "unchanged" (10), 1B 0x21 an empty installed list (12), 1D every generation 1 (13), or with a 1E's answer
// <slot> <result> (15).
int sysex_slot_empty(uint8_t* m, int slot);
int sysex_slot_unchanged(uint8_t* m, int slot);
int sysex_installed_empty(uint8_t* m);
int sysex_slots(uint8_t* m, int answer_slot, int result);

}  // namespace core
