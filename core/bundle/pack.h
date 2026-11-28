// core/bundle/pack.h - a box project's bundle as a zip (port step S8n, the portable part): the manifest and the
// entries laid out with zip.h, and the reader's checks of a zip against its manifest. No card I/O and no job here: the
// Pi's pack / unpack service (core 0) reads each entry's bytes for render() and feeds the import's bytes to EntryHash;
// what goes where on the card (the box folder's store/<project>/, /Samples) is that service's.
//
// Layout: entry 0 is manifest.json, then the manifest's entries in its order. Entry names by kind:
//   box/<name>          what the box's mod streamed over (the box project: kits, patterns, sounds)
//   store/<file>        a STORE record file of the project's folder (<scope>-<handle, 4 hex>.bin)
//   samples/<path>      a pool sample, <path> its card path under /Samples
// check() refuses a zip whose entries are not exactly the manifest's (an extra, a missing or a reordered entry, a
// size that differs, a name outside its kind's folder).
#pragma once
#include <stdint.h>

#include "../lib/sha256.h"
#include "manifest.h"
#include "zip.h"

namespace core {
namespace bundle {

constexpr int kEntriesMax = kZipEntriesMax - 1;   // the manifest's entries (manifest.json is the zip's first)

// A STORE record's / a sample's entry name; false: it does not make a bundle-safe name
bool store_entry_name(const char* record_file, char* out, int cap);   // "1-0001.bin" -> "store/1-0001.bin"
bool sample_entry_name(const char* card_path, char* out, int cap);    // "/Samples/Drums/k.wav" -> "samples/Drums/k.wav"

// The zip's entries for m (each MEntry's size and crc set) after manifest.json (mlen bytes, CRC mcrc); epoch: the
// time the zip records (seconds since 1970, UTC; 0 -> 1980-01-01). z holds at least m.ne + 1. false: too many entries,
// over 4 GiB, a bad name.
bool plan(const Manifest& m, uint32_t mlen, uint32_t mcrc, uint32_t epoch, ZEntry* z, int cap, Layout* L);

// A bundle's first bytes (at least the first local header, its name and padding, and the manifest): where manifest.json's
// bytes are (offset, size), so the registry's sniffer and read_manifest can run before the central directory is read.
// false: not a bundle (not a zip, or its first entry is not manifest.json, or the bytes stop short of its header).
bool sniff_zip(const uint8_t* head, uint32_t n, uint64_t* off, uint32_t* size);

// The read zip (z: central() and local() run on every entry) against the manifest read from its first entry: the
// same entries in the same order, the same sizes, names in their kind's folder. Sets each MEntry's crc from the zip.
bool check(Manifest* m, const ZEntry* z, int nz, char* err, int errcap);

// An entry's bytes as they arrive (any pieces): CRC-32 and SHA-256, then matched against the manifest's entry.
struct EntryHash {
  Sha256 s;
  uint32_t crc, n;
  void begin() { s.init(), crc = 0, n = 0; }
  void update(const void* p, uint32_t len) { s.update(p, len), crc = crc32(crc, p, len), n += len; }
  // false and err: its size, CRC or SHA-256 differs (the import writes nothing of a bundle with one wrong byte)
  bool match(const MEntry& e, char* err, int errcap);
};

}  // namespace bundle
}  // namespace core
