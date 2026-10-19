// core/host/platform_host.cpp - the Mac's platform functions and files (core/host/core_port.h).
#include <dirent.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include "platform.h"

namespace core {

static std::string g_card = "/tmp/core-card";
static void (*g_sink)(const char*, int) = nullptr;

void host_set_card(const char* dir) { g_card = dir; }
void host_set_log_sink(void (*sink)(const char*, int)) { g_sink = sink; }
std::string host_path(const char* p) { return g_card + (p[0] == '/' ? "" : "/") + p; }

static uint64_t ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
// tests and the emulator link: a clock set from outside (host_fake_time), else the monotonic clock
static bool g_fake = false;
static uint64_t g_fake_us = 0;
void host_fake_time(bool on, uint64_t us) { g_fake = on, g_fake_us = us; }
uint32_t now_us() { return g_fake ? (uint32_t)g_fake_us : (uint32_t)(ns() / 1000); }
uint32_t now_ms() { return g_fake ? (uint32_t)(g_fake_us / 1000) : (uint32_t)(ns() / 1000000); }
uint32_t cycles() { return (uint32_t)ns(); }
uint32_t cycles_per_second() { return 1000000000u; }
uint32_t random_seed() { return 1; }

void log(const char* fmt, ...) {
  char b[200];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > 199) n = 199;
  if (g_sink) g_sink(b, n);
  else fprintf(stderr, "core: %.*s\n", n, b);
}

uint32_t sample_ram_total() {
  const char* e = getenv("CORE_SAMPLE_RAM_MB");   // tests: 0 or unset = none (as a Teensy without PSRAM)
  return e ? (uint32_t)atoi(e) << 20 : 0;
}
void* sample_ram_alloc(uint32_t bytes) { return malloc(bytes); }
void sample_ram_free(void* p) { free(p); }

// ---- files
static constexpr int kReopen = 4;   // a copy of a kReadWrite handle: read and write, without truncating

File& File::operator=(const File& o) {
  if (this == &o) return *this;
  close();
  if (o.isOpen()) {
    open_full(o.path_, o.mode_ == kRead ? kRead : o.mode_ == kReadWrite ? kReopen : kAppend, o.index_);
    seekSet(o.pos_);
    next_ = o.next_;
  }
  return *this;
}

bool File::open_full(const std::string& p, int mode, uint32_t index) {
  close();
  std::string m = host_path(p.c_str());
  struct stat st;
  bool there = stat(m.c_str(), &st) == 0;
  if (there && S_ISDIR(st.st_mode)) {
    if (mode != kRead) return false;
    DIR* d = opendir(m.c_str());
    if (!d) return false;
    names_.clear();
    while (dirent* e = readdir(d))
      if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names_.push_back(e->d_name);
    closedir(d);
    std::sort(names_.begin(), names_.end());
    dir_ = true;
  } else {
    if ((mode == kRead || mode == kReopen) && !there) return false;
    static const char* const how[] = {"rb", "wb", "ab", "w+b", "r+b"};
    fp_ = fopen(m.c_str(), how[mode]);
    if (!fp_) return false;
    if (mode == kAppend) fseek(fp_, 0, SEEK_END);
    size_ = mode == kWrite || mode == kReadWrite ? 0 : (uint64_t)(there ? st.st_size : 0);
    pos_ = mode == kAppend ? size_ : 0;
  }
  path_ = p;
  mode_ = mode == kReopen ? kReadWrite : mode;
  index_ = index;
  next_ = 0;
  return true;
}

bool File::open(const char* path, int mode) { return open_full(path, mode, 0); }

bool File::open(File* dir, uint32_t index, int mode) {
  if (!dir || !dir->dir_ || index >= dir->names_.size()) return false;
  std::string p = dir->path_ + (dir->path_ == "/" ? "" : "/") + dir->names_[index];
  return open_full(p, mode, index);
}

bool File::openNext(File* dir, int mode) {
  if (!dir || !dir->dir_) return false;
  while (dir->next_ < dir->names_.size()) {
    uint32_t i = dir->next_++;
    if (open(dir, i, mode)) return true;
  }
  close();
  return false;
}

bool File::close() {
  bool was = isOpen();
  if (fp_) fclose(fp_);
  fp_ = nullptr;
  dir_ = false;
  names_.clear();
  size_ = pos_ = 0;
  return was;
}

bool File::getModifyDateTime(uint16_t* date, uint16_t* time) const {
  struct stat st;
  *date = *time = 0;
  if (!isOpen() || stat(host_path(path_.c_str()).c_str(), &st) != 0) return false;
  struct tm t;
  localtime_r(&st.st_mtime, &t);
  *date = (uint16_t)((t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday);
  *time = (uint16_t)(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2);
  return true;
}

size_t File::getName(char* out, size_t n) const {
  size_t s = path_.rfind('/');
  std::string base = s == std::string::npos ? path_ : path_.substr(s + 1);
  size_t k = base.size() < n - 1 ? base.size() : n - 1;
  memcpy(out, base.data(), k);
  out[k] = 0;
  return k;
}

int File::read(void* buf, size_t n) {
  if (!fp_ || (mode_ != kRead && mode_ != kReadWrite)) return -1;
  fseek(fp_, (long)pos_, SEEK_SET);
  size_t got = fread(buf, 1, n, fp_);
  pos_ += got;
  return (int)got;
}

int File::read() {
  uint8_t b;
  return read(&b, 1) == 1 ? b : -1;
}

size_t File::write(const void* buf, size_t n) {
  if (!fp_ || mode_ == kRead) return 0;
  if (mode_ == kAppend) pos_ = size_;   // as O_APPEND: every write at the end
  fseek(fp_, (long)pos_, SEEK_SET);
  size_t w = fwrite(buf, 1, n, fp_);
  fflush(fp_);
  pos_ += w;
  if (pos_ > size_) size_ = pos_;
  return w;
}

bool File::seekSet(uint64_t pos) {
  if (dir_) return pos == 0;
  if (!fp_ || pos > size_) return false;
  pos_ = pos;
  return true;
}

// ---- the card
bool Fs::begin() {
  struct stat st;
  return stat(g_card.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool Fs::mkdir(const char* p) {   // with its parents, as SdFat's mkdir (pFlag true)
  std::string full = host_path(p);
  for (size_t i = g_card.size() + 1; i <= full.size(); ++i)
    if (i == full.size() || full[i] == '/') {
      std::string part = full.substr(0, i);
      struct stat st;
      if (stat(part.c_str(), &st) != 0 && ::mkdir(part.c_str(), 0755) != 0) return false;
    }
  return true;
}
bool Fs::rename(const char* a, const char* b) { return ::rename(host_path(a).c_str(), host_path(b).c_str()) == 0; }
bool Fs::remove(const char* p) { return ::remove(host_path(p).c_str()) == 0; }
bool Fs::exists(const char* p) {
  struct stat st;
  return stat(host_path(p).c_str(), &st) == 0;
}

static void fnv(uint64_t* h, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; ++i) {
    *h ^= b[i];
    *h *= 0x100000001b3ull;
  }
}

static void walk(const std::string& mac, int depth, uint64_t* h, uint32_t* entries, uint32_t* hidden) {
  DIR* d = opendir(mac.c_str());
  if (!d) return;
  std::vector<std::string> names;
  while (dirent* e = readdir(d))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
  closedir(d);
  std::sort(names.begin(), names.end());
  for (const std::string& nm : names) {
    if (nm[0] == '.') {
      ++*hidden;
      continue;
    }
    struct stat st;
    std::string full = mac + "/" + nm;
    if (stat(full.c_str(), &st) != 0) continue;
    uint64_t size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    uint8_t mark = S_ISDIR(st.st_mode) ? 1 : 0;
    fnv(h, nm.data(), nm.size());
    fnv(h, &mark, 1);
    fnv(h, &size, sizeof size);
    ++*entries;
    if (mark && depth < 12) {
      walk(full, depth + 1, h, entries, hidden);
      fnv(h, "/", 1);
    }
  }
}

uint64_t Fs::tree_hash(const char* path, uint32_t* entries, uint32_t* hidden) {
  *entries = *hidden = 0;
  struct stat st;
  std::string m = host_path(path);
  if (stat(m.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
  uint64_t h = 0xcbf29ce484222325ull;
  walk(m, 0, &h, entries, hidden);
  return h;
}

}  // namespace core
