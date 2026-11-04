// core/kv/kv.h - the Sidecar's generic record store for Rytm mods (host-link 51 STORE, Storage phase 2026-11-04;
// sim/protocol.py "STORE"). The Pi keeps small records by (project, scope, handle) and knows nothing about what is in
// them: a mod on the Rytm (through the companion, sidecar-mods 0032) writes a record, gets a new handle back, and keeps
// that handle in its own core-ar2 field. Handles are versions: every write is a new record under a new handle, the old
// one stays until a KEEP (the full handle set a saved project still uses) drops the rest of that project.
//
// Layout on the card: /.rytm/store/<project id, 8 hex>/<scope>-<handle, 4 hex>.bin, written through the backend's
// atomic write (core/fmt write_atomic on the Pi's card core). Portable (Pi, Mac core, Linux tests): no card call here,
// only the Backend's. Not thread-safe: one core drives it (the Pi's core 0, from the host-link queue).
#pragma once
#include <stdint.h>

namespace core {
namespace kv {

constexpr int kScopes = 8;           // 0 project, 1 kit, 2 sound, 3 pattern, 4 track, 5 global, 6..7 spare
constexpr uint32_t kMaxRecord = 4096;  // bytes a record
constexpr int kMaxRecords = 1024;    // records a project
constexpr int kPathMax = 64;

// What the store needs from the card. The Pi glue maps these onto core 0's FatFs + core/fmt/write_atomic; the tests
// onto a map in memory.
struct Backend {
  virtual ~Backend() {}
  virtual bool write_atomic(const char* path, const uint8_t* d, uint32_t n) = 0;
  virtual int read(const char* path, uint8_t* d, uint32_t cap) = 0;   // bytes read, -1 absent or error
  virtual bool remove(const char* path) = 0;
  // every file name (not path) in dir, one call each; false: no such folder
  virtual bool list(const char* dir, void (*each)(const char* name, void* ctx), void* ctx) = 0;
};

enum Status : uint8_t { kOk = 0, kMissing, kNoProject, kFull, kCardError, kBad, kTooBig };
const char* status_name(int s);

// The store for one project at a time (the Rytm's). open() lists the project's folder once; read / write / keep use
// that index (no folder walk per call).
class Store {
 public:
  explicit Store(Backend* b) : b_(b) {}
  Status open(uint32_t project);   // HELLO: the project the Rytm has loaded (0: none, everything refused)
  uint32_t project() const { return project_; }
  int records() const { return n_; }
  // the record's bytes into d (at most cap), *n its length
  Status read(int scope, uint16_t handle, uint8_t* d, uint32_t cap, uint32_t* n);
  // a new record: *handle its new handle (never 0, never one the project holds)
  Status write(int scope, const uint8_t* d, uint32_t n, uint16_t* handle);
  // KEEP: every record of the project whose (scope, handle) is not in the list is removed; returns how many
  Status keep(const uint8_t* scopes, const uint16_t* handles, int count, int* removed);
  static const char* path(uint32_t project, int scope, uint16_t handle, char* out, int cap);

 private:
  int find(int scope, uint16_t handle) const;
  static void add_name(const char* name, void* ctx);
  Backend* b_;
  uint32_t project_ = 0;
  int n_ = 0;
  uint16_t next_ = 1;
  struct Rec { uint8_t scope; uint16_t handle; } recs_[kMaxRecords];
};

// ---- the host-link codec (51 STORE, version 1). Every message F0 7D 52 54 51 01 <op> <body> F7; 7-bit bytes only.
// Values: a 16-bit handle as 3 bytes (h >> 14, h >> 7 & 7F, h & 7F); a 32-bit project as 5; lengths and offsets as 2
// (14 bits); data as 7-in-8 groups (every 7 bytes -> 8: a byte of high bits first, bit i = byte i's bit 7).
//   Rytm -> Pi  00 HELLO <project 5>
//               01 READ <seq> <scope> <handle 3>
//               02 WRITE <seq> <scope> <total 2> <offset 2> <data>   chunks in order; the last completes the record
//               03 KEEP <seq> <final 0/1> <n> {<scope> <handle 3>} x n   repeated until final = 1
//               04 NAME <project 5> <len> <name, len bytes>   the project's name (up to 16, 7-bit): after each HELLO's
//                  answer and on a change; no answer. The control core keeps it (link.h), for the log, /status, the web
//   Pi -> Rytm  40 HELLO <service version> <records 2>
//               41 DATA <seq> <scope> <handle 3> <total 2> <offset 2> <data>
//               42 WRITTEN <seq> <scope> <handle 3>
//               43 KEPT <seq> <removed 2>
//               7F STATUS <seq> <code>   anything refused (kv::Status); seq 7F when there was none
enum Op : uint8_t { kOpHello = 0x00, kOpRead = 0x01, kOpWrite = 0x02, kOpKeep = 0x03, kOpName = 0x04,
                    kOpHelloAck = 0x40, kOpData = 0x41, kOpWritten = 0x42, kOpKept = 0x43, kOpStatus = 0x7F };
constexpr uint8_t kCmd = 0x51, kVer = 1;
constexpr int kChunk = 112;   // raw bytes a DATA / WRITE message carries (128 on the wire)

int pack7(const uint8_t* in, int n, uint8_t* out);     // returns bytes written: n + ceil(n / 7)
int unpack7(const uint8_t* in, int n, uint8_t* out);   // returns bytes written, -1 malformed

// The service: takes a whole 51 message (F0..F7), answers through out(). One per engine; WRITE assembles its chunks
// in a buffer of its own (one write in flight, as the companion sends them).
class Service {
 public:
  Service(Store* s, void (*out)(const uint8_t* m, int n)) : s_(s), out_(out) {}
  void sysex(const uint8_t* m, int n);

 private:
  void send(const uint8_t* body, int n);
  void status(uint8_t seq, uint8_t code);
  Store* s_;
  void (*out_)(const uint8_t*, int);
  uint8_t wbuf_[kMaxRecord];
  uint32_t wtotal_ = 0, wgot_ = 0;
  int wscope_ = -1, wseq_ = -1;
  uint8_t kscope_[kMaxRecords];
  uint16_t khandle_[kMaxRecords];
  int kn_ = 0;
  bool kbad_ = false;
};

}  // namespace kv
}  // namespace core
