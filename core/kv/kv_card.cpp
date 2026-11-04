// core/kv/kv_card.cpp - see kv_card.h.
#include "kv_card.h"

#include "../fmt/fmt.h"
#include "../platform.h"

namespace core {
namespace kv {

bool CardBackend::write_atomic(const char* path, const uint8_t* d, uint32_t n) {
  static const uint8_t kEmpty = 0;
  return fmt::write_atomic(path, n ? d : &kEmpty, n);   // creates the project's folder when it is missing
}

int CardBackend::read(const char* path, uint8_t* d, uint32_t cap) {
  fmt::recover(path);   // a cut inside the rename window: the .prev back
  File f;
  if (!f.open(path, core::kRead)) return -1;
  uint64_t size = f.fileSize();
  if (size > cap) { f.close(); return -1; }
  int got = size ? (int)f.read(d, (size_t)size) : 0;
  f.close();
  return got == (int)size ? got : -1;
}

bool CardBackend::remove(const char* path) {
  Fs fs;
  return fs.remove(path);
}

bool CardBackend::list(const char* dir, void (*each)(const char* name, void* ctx), void* ctx) {
  File d;
  if (!d.open(dir, core::kRead) || !d.isDir()) return false;
  char name[64];
  bool isdir = false;
  uint64_t size = 0;
  uint16_t fd = 0, ft = 0;
  while (d.nextEntry(name, sizeof name, &isdir, &size, &fd, &ft))
    if (!isdir) each(name, ctx);
  bool ok = !d.nextFailed();
  d.close();
  return ok;
}

}  // namespace kv
}  // namespace core
