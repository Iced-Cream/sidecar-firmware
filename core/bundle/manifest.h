// core/bundle/manifest.h - a box project's bundle manifest (manifest.json, the zip's first entry; port step S8n,
// decided 2026-11-28 in the generic two-box scope, section 8 "Bundles"). One bundle per box project: what the box's
// own mod streamed over (kind "box": kits, patterns, sounds, opaque to the Pi), the project's STORE folder (kind
// "store", one entry a record file) and the pool samples it uses (kind "sample", with the card paths they came from).
// The blobs and web bundles the project needs are listed by hash only ("needs": never packed; the import asks for any
// the Pi lacks).
//
// Versions come from the format registry (core/fmt, formats.cpp "bundle"): the manifest's "version" is the registry's
// current, "min_reader" what the content needs, and read_manifest() takes its verdict from fmt::load() (newer with a
// min_reader past this build: refused; newer but readable: read, read_only set; the old line's version 1 bundles,
// Rytm projects with slots and prints, are a different content and refused as an older version). An entry's own file
// format (STORE records: "store") is checked against its registry row too: a record newer than this build reads
// refuses the bundle, an unregistered name (pool, code, web formats until their steps land) is carried as it is.
//
// Untrusted text: the reader is json.h's bounded one, at most kManifestMax bytes, names and paths checked (zip.h
// name_ok), no allocation past alloc().
#pragma once
#include <stdint.h>

#include "json.h"
#include "zip.h"

namespace core {
namespace bundle {

constexpr uint32_t kManifestMax = 1u << 20;
constexpr int kCardPath = 201;       // a card path (fsweb's 200 + NUL)
constexpr int kNeedsMax = 64;        // blobs + web bundles a project names
constexpr int kHexId = 8;            // a box UID / a project ID: 8 lowercase hex digits

enum Kind : uint8_t { kKindBox = 0, kKindStore, kKindSample, kKinds };
const char* kind_name(int k);        // "box" "store" "sample"
int kind_of(const char* s);          // -1 unknown

enum NeedKind : uint8_t { kNeedCode = 0, kNeedWww, kNeedKinds };
const char* need_name(int k);        // "code" "www"
int need_of(const char* s);

struct MEntry {
  char name[kNameMax + 1];           // in the bundle
  uint8_t kind;
  uint32_t size, crc;                // crc: the zip's (not in the JSON; set by the packer, checked by the reader)
  uint8_t sha[32];
  int first_path, npaths;            // its card paths in Manifest::p (samples)
  char fmt[16];                      // the file's format name ("store", ...), "" not known
  int fmt_ver;                       // and its version (0 unknown)
};
struct MPath { char path[kCardPath]; int entry; };
struct Need { uint8_t kind; char name[32]; uint8_t sha[32]; };

struct Manifest {
  int version, min_reader;
  bool read_only;                    // read_manifest: a newer bundle this build may read (fmt kForeignNewer)
  char created[24], clock[12], sc_version[32], board[8];
  char box_type[8];                  // "rytm" "a4" "dt" (core/device's type folder without the dot)
  char box_uid[kHexId + 1];          // DeviceUID, 8 lowercase hex
  char box_name[24];                 // the box folder's name.txt (23 characters)
  char project_id[kHexId + 1];       // the STORE project ID (/store/<id>/), 8 lowercase hex
  char project_name[24];             // the box project's name as the box shows it
  Need needs[kNeedsMax];
  int nneeds;
  MEntry* e;
  int ne, cap_e;
  MPath* p;
  int np, cap_p;
  char (*missing)[kCardPath];        // pool samples the project names that were not on the card
  int nmissing, cap_missing;

  void alloc(int entries, int paths, int missing);   // once (the caller frees nothing: one manifest per job)
  void clear();
  int add_entry(const char* name, int kind);          // -1 full or a bad name
  bool add_path(int entry, const char* path);         // the entry's paths are added together (after it)
  bool add_need(int kind, const char* name, const uint8_t sha[32]);   // false: full, a bad name, the hash twice
  bool add_missing(const char* path);
  int find_entry(const char* name) const;
  int find_path(const char* path) const;              // case ignored (FAT): the MPath index
  uint64_t bytes() const;
  int count(int kind) const;
};

// The manifest's JSON (entries in m.e order). "version" and "min_reader": the registry's "bundle" row (current; a
// writer writes the current version only, m.version / m.min_reader are not used). false: no room, or a field the
// reader would refuse (a box UID or project ID not 8 hex digits, a bad box type, an entry newer than its format's row).
bool write_manifest(const Manifest& m, JOut& o);
// Untrusted text in: false and err when the registry refuses it or any field is wrong (names, sizes, hashes, nesting)
bool read_manifest(const char* text, uint32_t n, Manifest* m, char* err, int errcap);

bool hex_id(const char* s);          // exactly 8 lowercase hex digits

}  // namespace bundle
}  // namespace core
