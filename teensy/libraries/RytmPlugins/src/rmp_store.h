// rmp_store.h - installed packages as files /machines/<id>.rmp in a LittleFS
// in the Teensy's program flash (works without an SD card). One version per id
// in this sub-project; the companion (sub-project 3) may keep several.
//
// WARNING: erasing/programming program flash stalls code execution (the flash is
// also where the code runs from; measured on rmp_test: see its serial output).
// The core's eeprom.c erases and programs from RAM with interrupts masked until the
// flash is ready: a 64 KB block erase (LittleFS allocating a block, or compacting a
// directory) holds them off ~0.25-0.28 s (measured with CYCCNT), so millis()/micros() lose that time (time
// across an install with ARM_DWT_CYCCNT) and nothing is serviced meanwhile: the USB
// host build's Overbridge output has a gap of about the erase time. Audio drops
// during an install, so install ONLY when audio may drop (stopped transport / no
// voice playing), never during play: voiceloop refuses an install from the card
// while the transport runs (STOP TO INSTALL) and pauses a copy under way. The
// companion/installer must respect this. Reads (rmp_store_read, rmp_store_list)
// are safe at any time. Every erase feeds the watchdog first (rmp_store.cpp).
//
// Limits: an install is capped at RMP_STORE_MAX_PKG (64 KB: the code arena is 32 KB,
// plus relocations and manifest). Commit verifies in the flash file with a streaming
// CRC (512-byte stack chunks, no malloc). LOADING streams too: rmp_slots reads the
// package with rmp_store_size + rmp_store_read_at (no whole package in RAM).
//
// Rules: main loop only, never from an ISR or the audio callback; one install at a
// time. The store is a LittleFS of RMP_STORE_BYTES at the top of program flash, so a build with another
// RMP_STORE_BYTES puts its base elsewhere: LittleFS 2.4's mount does not check the superblock's block count, and
// the core's LittleFS_Program::begin formats whenever a mount fails. Without a guard, a build with another size
// may MOUNT STALE DATA (a smaller store's base lies inside a larger one: an old store's leftovers, which it then
// writes into the larger store's blocks) or reformat (wipe) the store. rmp_store_begin therefore probes the raw
// flash first (rmp_store_probe.h) at our base and at every RMP_STORE_KNOWN_SIZES base: it mounts only our own
// superblock (littlefs 2.x, our block size and count) with no larger store's metadata below it (a CRC-valid commit
// at its {0, 1}, superblock or not), formats only a
// region that is entirely erased (first boot), and otherwise mounts nothing and formats nothing
// (rmp_store_foreign(): "foreign store layout", slots MISSING) until an explicit rmp_store_wipe (voiceloop's
// 'F' "WIPE" frame, tools/rmp_install.py wipe). A build WITHOUT this guard (anything before the user-slots merge of
// 2026-10-11, e.g. 9870991 and older: 1 MB at 0x606C0000) still mounts or formats inside this store: never flash
// one on a Teensy holding this store.
//
// Errors: a package whose id is unsafe as a file name (empty, >31 chars, not
// NUL-terminated, '/', '\\', leading '.', or "..") is refused with RMP_ERR_FORMAT.
// Storage failures (short read, rename failure, size mismatch) are
// reported as RMP_ERR_SIZE.
#pragma once
#include <stdint.h>
#include "rmp_loader.h"
// 4 MB: 64 blocks of 64 KB at the top of program flash (the core's LittleFS_Program: 0x60000000 + 0x7C0000 -
// 4 MB = 0x603C0000 up to the EEPROM emulation's 256 KB). The program image must end below it (run_sketch.sh
// checks); an upload rewrites only the image, so the store survives it. Every build that mounts the store
// (voiceloop serial and host, rmp_test) takes this value: do not override it per build. Was 1 MB (16 blocks,
// the top 1 MB of this region) until 2026-10-10; an unguarded build with the 1 MB value (pre-merge) mounts
// whatever superblock pair is at 0x606C0000 - the old 1 MB store's stale leftovers in this store's blocks - and
// writes there, or formats its own pair there; either corrupts this store (which took the old 1 MB store's blocks
// as free). With the guard (above) a 1 MB build sees this store's superblock at 0x603C0000 and mounts nothing.
#ifndef RMP_STORE_BYTES
#define RMP_STORE_BYTES (4u * 1024u * 1024u)
#endif
// Every store size a build has used: rmp_store_probe looks for a superblock at each one's base (ours included or
// not). Add a size here before a build with it is flashed.
#define RMP_STORE_KNOWN_SIZES (1u * 1024u * 1024u), (4u * 1024u * 1024u)
#define RMP_STORE_MAX_PKG (64u * 1024u)
// LittleFS's metadata_max: a directory's commit log is compacted once it passes this (the Teensy 4.1's
// blocks are 64 KB; unbounded, the log grew to tens of KB and every lookup scanned it - rmp_store.cpp).
// Its half caps a compacted directory before LittleFS splits it into another pair (2 more 64 KB blocks):
// 2 KB holds the 32 packages the Rytm's list shows.
#ifndef RMP_STORE_META_MAX
#define RMP_STORE_META_MAX 4096u
#endif
// Mount: probe the raw flash, mount our own store, format an erased region (first boot), leave anything else alone
// (false, rmp_store_foreign() true). Idempotent; removes a leftover install file.
bool rmp_store_begin();
bool rmp_store_foreign();                                // the probe found a foreign/mismatched layout: not mounted
void rmp_store_describe(char *buf, uint32_t n);          // the probe's findings, one line (for the boot log)
// Explicit wipe: erase a larger known store's superblock pair below ours (outside our region, above the image),
// format our region, mount. Every package is lost. Erases ~0.25-0.28 s a block: not while audio plays.
bool rmp_store_wipe();
// Called before every flash erase (after the watchdog is fed), from the main loop: a weak no-op here; voiceloop's
// keeps its cycle-counter clock (plug_ms) across the erases of one long operation.
void rmp_store_on_erase();
// An install in chunks: begin with the total size, write in order, then commit
// (verify the whole package, rename over the old one). Abort leaves the old one.
bool rmp_store_install_begin(uint32_t total);
// install_write: false if the chunk would exceed the declared total (install stays open),
// or if the flash write fails (the install is then aborted: TMP removed).
bool rmp_store_install_write(const uint8_t *p, uint32_t n);
bool rmp_store_pending();                                // a leftover/unfinished install file exists
rmp_status rmp_store_install_commit(char *id_out);       // id_out: 32 bytes
void rmp_store_install_abort();
bool rmp_store_remove(const char *id);                   // false for an unsafe id or a missing package
// Read a package into buf (len in/out). false when missing or unsafe id; when buf is too
// small it returns false and sets *len to the size needed.
bool rmp_store_read(const char *id, uint8_t *buf, uint32_t *len);
// Streaming access for loading (rmp_slots' size_of/read_at): each call seeks and reads on the
// cached handle (opened on first use). false when missing, unsafe id, or (read_at) any part of
// [off, off + n) is past the end / a short read.
bool rmp_store_size(const char *id, uint32_t *len);
bool rmp_store_read_at(const char *id, uint32_t off, uint8_t *buf, uint32_t n);
// The streaming access keeps ONE file open (the last id used; an open costs ~0.1-0.4 ms on the
// program-flash LittleFS with the metadata log bounded, a 512 B read ~30 us). Installs, removes and begin drop it;
// rmp_store_release() closes it (call it when a load is done: an open file holds a cache
// buffer).
void rmp_store_release();
// List installed ids (calls f for each).
void rmp_store_list(void (*f)(const char *id, uint32_t size, void *ctx), void *ctx);
// Counters of the streaming access above (opens, reads, bytes, microseconds), for
// measuring a load: reset, load, read.
struct rmp_store_stats_t { uint32_t opens, reads, bytes, us_open, us_read; };
void rmp_store_stats_reset();
rmp_store_stats_t rmp_store_stats();
// Size of the store and the bytes in use (whole LittleFS blocks), for the device-info report.
// usedSize() walks the file system, a few ms: not while audio is critical.
bool rmp_store_space(uint32_t *total, uint32_t *used);
// The listing one entry a call: begin, then next until it returns false (it closes itself then);
// iter_end abandons it. One iteration at a time; id is 32 bytes.
bool rmp_store_iter_begin();
bool rmp_store_iter_next(char *id, uint32_t *size);
void rmp_store_iter_end();
