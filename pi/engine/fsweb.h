// pi/engine/fsweb.h - the web page's file browser (sidecar.local, docs/user-machines-pi.md "File browser"): list,
// download in ranges, upload in chunks, delete, new folder, rename, free space - over a card Backend (FatFs on the
// Pi: web.cpp; stdio in pi/engine/tests/test_fsweb.cpp). Portable (hosted flags): no Circle, no allocation.
//
// The safe write path (as sdfiles' PUT): an upload goes to <path>.part, chunk by chunk, each chunk read back and
// compared, a running CRC-32 kept; the last chunk's CRC must equal the client's, then <path>.part replaces <path>.
// A failure leaves <path> as it was. Boot files (config.txt, cmdline.txt, sdboot*, engine.img*, kernel*.img,
// start*.elf, fixup*.dat, bootcode.bin, armstub*, tryboot.txt*, *.dtb, overlays/) are read-only here: listed with
// "ro":1, never written, deleted or renamed (sdfiles on TCP 7310 installs them). A package for /Plugins (*.rmp) is
// checked before it goes in place (header, size, CRC, ABI version, target): a bad one is refused with the reason.
// Chunks are written in 4 KB pieces with the backend's yield() between them, so one request never holds the card
// (or core 0's scheduler) for long.
#ifndef _pi_engine_fsweb_h
#define _pi_engine_fsweb_h
#include <stdint.h>

namespace fsweb {

constexpr int kPath = 200;               // the longest path
constexpr uint32_t kChunkMax = 64 * 1024;   // the largest upload chunk (the page sends 48 KB)

struct Entry { char name[96]; uint64_t size; bool dir; };

struct Backend {
  bool (*stat)(const char* path, uint64_t* size, bool* dir);
  int (*list)(const char* path, Entry* out, int max, bool* more);     // -1: not a folder
  bool (*read)(const char* path, uint64_t off, void* buf, uint32_t n, uint32_t* got);
  bool (*write_at)(const char* path, uint64_t off, const void* buf, uint32_t n, bool create);   // create: truncate first
  bool (*remove)(const char* path);   // a file or an empty folder
  bool (*rename)(const char* from, const char* to);   // to must not exist
  bool (*mkdir)(const char* path);
  bool (*space)(uint64_t* free, uint64_t* total);
  void (*yield)();                    // between 4 KB pieces (Circle: the scheduler)
  void (*changed)(const char* path);  // a file under /Plugins changed (the engine's rescan); may be null
};

struct Result { int status; char msg[160]; };   // status: an HTTP code (200 ok)

bool path_ok(const char* p);          // absolute, printable ASCII, no "..", "//", '\\' or ':', at most kPath
bool read_only(const char* p);        // a boot file (or the root): see above
bool is_package(const char* p);       // /Plugins/<name>.rmp
// The Sidecar's own folders (/.rytm, /Samples, /Plugins): never deleted or renamed here (their contents may be);
// listed with "ro":2. config.txt.bak (the restore backup) is a boot file (config.txt.prev stays writable).
bool kept_dir(const char* p);

// The JSON of a folder: {"path":..,"free":..,"total":..,"more":0|1,"entries":[{"n":..,"d":0|1,"s":..,"ro":0|1}]}
int list_json(const Backend& b, const char* path, char* out, int cap, Result* r);
// Upload chunk: data[0..n-1] at offset off of the file `path`, total bytes in all; crc the whole file's CRC-32
// (checked with the last chunk). 200 "ok <next offset>" or "done <bytes>"; 409 "expected <offset>" when off is not
// where the upload stands (resume from there; off 0 always starts over); 400/403/507 with the reason.
void put(const Backend& b, const char* path, uint64_t off, uint64_t total, uint32_t crc, const uint8_t* data, uint32_t n,
         Result* r);
void remove(const Backend& b, const char* path, Result* r);
void mkdir(const Backend& b, const char* path, Result* r);
// mkdir -p (the page's folder upload): every missing folder on the way; 200 "made" or "exists", 409 when a file
// is in the way.
void mkdirs(const Backend& b, const char* path, Result* r);
void rename(const Backend& b, const char* from, const char* to, Result* r);
// A package's check (the last chunk of an upload to /Plugins, and the tests): its .part file through b.read;
// nullptr when good, else the reason.
const char* check_package(const Backend& b, const char* path, uint64_t size);
uint32_t crc32(uint32_t crc, const uint8_t* p, uint32_t n);   // zlib's (start 0)

}  // namespace fsweb
#endif
