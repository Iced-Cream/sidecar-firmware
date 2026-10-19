// rmp_proto.h - the user-slot messages between the Rytm and the Teensy (2026-10-10; sim/protocol.py,
// docs/superpowers/specs/2026-10-10-user-slots-rytm-design.md 2.1). All F0 7D 52 54 <cmd> <ver> ... F7,
// version RMP_PROTO_VER right after the command; a receiver ignores another version or command.
//   19 <ver> <slot 1..5|0> <gen> <what> <arg>          Rytm -> Teensy  ASK (11 bytes)
//   1A <ver> <slot> <gen> <part> ...                   Teensy -> Rytm  METADATA
//        part 0x7F unchanged (10 bytes); part 0: <state> <flags> <nknobs> <nparts> <name 10> <short 4>
//        <p2 title 13> <fp hi> <fp lo> (43 bytes; fp: the slot's rmp_plugin_fp when READY, else 0 - spec 2.6;
//        an older Teensy's 41-byte part 0 reads as fp 0); parts 1..nparts-1: up to 4 knobs of 24 bytes: <p> <look> <kind> <role>
//        <min> <max> <default> <enum count> <long 12> <short 4> (at most 106 bytes)
//   1B <ver> <slot> <list> <gen> <replace> <total> {<index> <13 name bytes>} x <= 8   LIST NAMES (<= 124)
//   1C <ver> <voice> <slot> <n> {<knob 0..15> <value 0..127>}                         KNOBS (<= 42)
//   1D <ver> <gen U1..U5> <installed gen> [<slot> <result>]                           SLOTS (13 / 15)
//   1E <ver> <slot 1..5> <op> <arg> <installed gen>    Rytm -> Teensy  ASSIGN (11 bytes)
// Text: ASCII 0x20..0x7e (else '_'), NUL padded. Generations 1..127 per slot; 0: the Rytm knows nothing.
#pragma once
#include <stdint.h>
#include "rmp_manifest.h"
#include "rmp_slots.h"
#define RMP_PROTO_VER 1
enum { RMP_CMD_ASK = 0x19, RMP_CMD_META = 0x1A, RMP_CMD_LIST = 0x1B, RMP_CMD_KNOBS = 0x1C, RMP_CMD_SLOTS = 0x1D,
       RMP_CMD_ASSIGN = 0x1E };
enum { RMP_ASK_META = 0, RMP_ASK_ENUM = 1, RMP_ASK_PRESETS = 2, RMP_ASK_INSTALLED = 3 };
enum { RMP_LIST_PRESETS = 0x20, RMP_LIST_INSTALLED = 0x21 };   // 0x00..0x0F: knob k's enum names
// the protocol's slot states (not rmp_slot_state's numbering: rmp_proto_state maps)
enum { RMP_PST_EMPTY = 0, RMP_PST_READY, RMP_PST_LOADING, RMP_PST_MISSING, RMP_PST_BAD, RMP_PST_NO_ROOM, RMP_PST_MUTED };
enum { RMP_FLAG_PARAMS = 1, RMP_FLAG_ON_CARD = 2, RMP_FLAG_PRESETS = 4 };
enum { RMP_RES_READY = 0, RMP_RES_NOT_INSTALLED, RMP_RES_NO_ROOM, RMP_RES_STOP_TO_INSTALL, RMP_RES_NOT_ON_CARD,
       RMP_RES_BAD, RMP_RES_LOADING };
enum { RMP_OP_ASSIGN = 0, RMP_OP_CLEAR = 1, RMP_OP_INSTALL = 2 };
#define RMP_META_UNCHANGED 0x7F
#define RMP_LIST_PER_MSG 8
#define RMP_KNOBS_PER_PART 4
#define RMP_MSG_MAX 128   // the longest message built here is 124 bytes (1B)
struct rmp_ask { uint8_t slot, gen, what, arg; };              // slot 1..5, 0 for the installed list
struct rmp_assign { uint8_t slot, op, arg, installed_gen; };   // slot 1..5
int rmp_proto_state(rmp_slot_state s);
// The sound tag's plugin fingerprint (spec 2.6, sim/protocol.py plugin_fp): FNV-1a32 of the package id's bytes
// as stored (case kept, no version, up to its NUL) mod 16383, + 1: 1..16383 (0 is an untagged sound).
uint16_t rmp_plugin_fp(const char *id);
// A whole message m[0..n-1] (F0 .. F7): true and *a for a well-formed 19 / 1E of version 1 (19: what
// INSTALLED with slot 0 only, the others with slot 1..5).
bool rmp_parse_ask(const uint8_t *m, int n, rmp_ask *a);
bool rmp_parse_assign(const uint8_t *m, int n, rmp_assign *a);
int rmp_meta_nparts(const rmp_desc *d);   // 1 + ceil(nknobs / 4); 1 for d == nullptr
// Builders: out holds RMP_MSG_MAX bytes; each returns the message's length (0: no such part). Every
// caller-supplied byte is masked to 7 bits; total and n are clamped to their ranges (negative: 0).
int rmp_build_meta_unchanged(uint8_t *out, int slot, int gen);
// d null: no names, no knobs. fp (14 bits) goes out only with state RMP_PST_READY; 0 in every other state.
int rmp_build_meta_head(uint8_t *out, int slot, int gen, int state, int flags, const rmp_desc *d, int fp);
// Why a slot is not READY. The Rytm shows a non-READY slot's 1A name as "!" + name (host data: the
// Rytm build is unchanged), so the host puts the reason in the name: rmp_why_name gives "<TAG> <short>"
// in at most 9 characters ("ROOM NPPR" -> the Rytm shows "!ROOM NPPR"), the short name being the
// package's 4-character one (or the name's first 4). MUTED keeps the reason it was muted for.
enum rmp_why { RMP_WHY_NONE = 0, RMP_WHY_ROOM, RMP_WHY_CRASH, RMP_WHY_TIME, RMP_WHY_MISS, RMP_WHY_BAD, RMP_WHY_SAFE };
const char *rmp_why_tag(int why);              // "ROOM" "CRSH" "TIME" "MISS" "BAD" "SAFE"; "" for NONE
// The reason for a protocol state: NO_ROOM ROOM, MISSING MISS, BAD BAD, MUTED muted (CRASH when NONE),
// any other state NONE.
int rmp_why_of(int pstate, int muted);
// out[10]: "<TAG> <short>" (NUL-terminated, <= 9 characters); "" when why is NONE.
void rmp_why_name(char out[10], int why, const rmp_desc *d);
// rmp_build_meta_head with the reason in the name (why NONE: the same message as rmp_build_meta_head).
int rmp_build_meta_head_why(uint8_t *out, int slot, int gen, int state, int flags, const rmp_desc *d, int fp, int why);
int rmp_build_meta_part(uint8_t *out, int slot, int gen, int part, const rmp_desc *d);               // part 1..nparts-1
// 1026: the knobs a machine does not use, sent as unused (the Rytm draws them as its stock greyed "unused" boxes, not
// as P11 ...): every P the description lacks (P1..P8, and P9..P16 when page2 holds knobs) added in p order with an
// empty long and short name and 0 everywhere else (provisional contract: an empty label = unused; rytm-mods 0021).
// In place, for a copy to send in 1A (pre is moved with its knob). Returns how many were added.
int rmp_desc_pad_unused(rmp_desc *d);
// count (<= 8) entries first..first+count-1, names[i] at names + i * stride (NUL-terminated or 13+ bytes)
int rmp_build_list(uint8_t *out, int slot, int list, int gen, int replace, int total, int first, int count,
                   const char *names, int stride);
int rmp_build_knobs(uint8_t *out, int voice, int slot, int n, const uint8_t *knob, const uint8_t *val);   // n <= 16
int rmp_build_slots(uint8_t *out, const uint8_t gen[5], int installed_gen, int slot, int result);       // slot 0: no pair
