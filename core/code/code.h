// core/code/code.h - shipped code on the Pi (design "Sidecar code shipping", 2026-11-07; phase 1, the loader): a Rytm
// client mod carries a native AArch64 blob (SCX, core/code/scx.h), the companion offers it by its SHA-256, the Pi keeps
// it on its card by that hash and loads it into the DSP library as one more block, which graphs PLACE by name (core/dsp)
// like a built-in. The engine stays generic: one loader, nothing per mod.
//
// Host link (51 STORE, version 1, beside the DSP ops 30..32; all bytes 7-bit):
//   Rytm -> Pi  33 OFFER  <seq> <owner> <sha256: 32 bytes 7-in-8 = 37> <size 5> <name 8>   answer 73, 74 or 75
//               34 CHUNK  <seq> <owner> <offset 5> <data: up to 112 bytes 7-in-8>          no answer; a gap asks 73
//               35 COMMIT <seq> <owner>                                                     answer 74 or 75 (or 73)
//               36 DROP   <owner> <name 8>                                                  no answer
//   Pi -> Rytm  73 WANT     <seq> <owner> <offset 5>   send CHUNKs from offset (0: from the start)
//               74 LOADED   <seq> <owner> <name 8>     PLACE <name> works for this owner now
//               75 REFUSED  <seq> <owner> <code>       3 full, 5 bad, 9 abi, 10 hash mismatch, 11 import, 12 name clash,
//                                                      13 crashed before
// 32-bit values (size, offset) as 5 bytes (v >> 28, v >> 21 & 7F, v >> 14 & 7F, v >> 7 & 7F, v & 7F), as 51's project
// ID. Names are upper case ASCII, NUL padded to 8.
//
// Fast path: an OFFER whose hash is loaded answers 74 at once (control core); one the card holds is read, checked and
// loaded without a transfer. A transfer resumes from the WANT offset within one session; the companion's HELLO drops
// a transfer under way and forgets which owner offered what (blobs stay loaded, and on the card).
//
// Card (the card core's, through kv::Backend): /.rytm/code/<sha256 hex>.scx, written atomically once a transfer is
// committed and its hash and format check; /.rytm/code/index.txt, one line a blob (hash size name last bad), rebuilt
// from the folder when missing; a 16 MB cap (set_cache_cap), oldest offered evicted first, never one offered this boot.
// Crash mark /.rytm/code/.armed: the hashes whose code has run for less than a second (constructors at load, then the
// first second after the first PLACE). A boot that finds it marks those hashes bad: refused with 13 until a new version.
//
// Memory: one code arena carved at boot (8 MB on the Pi), 64 KB a unit, a blob in whole units. The board makes a blob's
// units writable to place it, writable + executable while its constructors run, then read-only + executable; a freed
// blob's units go back to writable, never executable (Board::protect).
//
// Threads: from_host / poll / take_answer / hello on the control core (with core/dsp); card_* on the card core.
// Without a card core (tests) everything runs on one thread. Not in this phase: the companion's code service, the slot
// trig service, LFI sandboxing, a fault or CPU guard per slot.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../kv/kv.h"
#include "loader.h"

namespace core {
namespace code {

enum Op : uint8_t { kOffer = 0x33, kChunk = 0x34, kCommit = 0x35, kDrop = 0x36, kWant = 0x73, kLoaded = 0x74, kRefused = 0x75 };
constexpr uint32_t kUnit = 64u << 10;          // the arena's unit, the Pi's page size (64 KB granule)
constexpr size_t kArenaBytes = 8u << 20;       // the Pi's code arena
constexpr uint64_t kCacheCap = 16ull << 20;    // the card cache's default cap
constexpr int kBlobs = 16;                     // loaded at once (= dsp::kShipped)
constexpr int kIndexMax = 256;                 // blobs the card index holds

// What the board gives the loader (the Pi: page tables and caches, pi/engine/code_mmu.cpp). null: nothing to do (a
// test whose arena is already readable, writable and executable).
enum Prot : uint8_t { kProtRW = 0, kProtRWX = 1, kProtRX = 2 };
struct Board {
  // [p, p + n) (whole units) to prot; for kProtRWX / kProtRX also cleans the data cache and invalidates the
  // instruction cache over it. False: could not.
  bool (*protect)(void* p, size_t n, Prot prot);
};

// Boot, control core, before the first message: the arena (unit aligned, a multiple of kUnit), two blob buffers of
// kMaxBlob each (the card core's transfer and load buffers), the board. False: refused (alignment, sizes).
bool begin(void* arena, size_t arena_bytes, uint8_t* buf_a, uint8_t* buf_b, const Board* board,
           kv::Backend* card = nullptr);   // null: the board's card (kv::CardBackend)
void set_cache_cap(uint64_t bytes);

bool is_code_op(uint8_t op);
// control core: a whole 51 message with a code op. 0: done here (answered or dropped); 1: for the card core
// (kv::link queues it to card_op); -1: malformed (dropped, counted).
int from_host(const uint8_t* m, int n);
void hello();                               // control core: the companion's HELLO
void poll();                                // control core, often: loads what the card core read, drops, crash marks
int take_answer(uint8_t* out, int cap);     // control core: the next answer, whole F0..F7; 0: none

// card core
bool card_can_take(const uint8_t* m, int n);   // false: the op must wait (the load buffer is still the control core's)
void card_op(const uint8_t* m, int n);
void card_hello();                             // the companion's HELLO reached the card core: a transfer is dropped
void card_poll();                              // often: crash marks, the index, a stalled transfer asked again

// /status, tests
struct BlobView { const char* name; const uint8_t* hash; int owner; int users; uint32_t bytes; };
int loaded();
bool blob(int i, BlobView* v);
struct Stats {
  uint32_t offers, fast, from_card, chunks, commits, loaded, refused, drops, unloaded, wants, ops_bad;
  int last_refused;
  uint32_t card_blobs;
  uint64_t card_bytes;
};
Stats stats();

// for tests: the card side's state forgotten (as at a boot), so the next card call reads the card again
void card_reboot_for_test();

}  // namespace code
}  // namespace core
