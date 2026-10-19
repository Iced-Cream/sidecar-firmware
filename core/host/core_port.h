// core/host/core_port.h - the Mac's (POSIX) port of the engine core (core/platform.h), for the tests and
// sim/coreengine.py. The card is a folder (host_set_card); paths are the card's ("/Samples/x.wav").
// Directory entries come in name order, each entry's dirIndex() its place in that order.
#pragma once
#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

#define CORE_COLD
#define CORE_RAM2
#define CORE_STR(s) (s)
#define sniprintf snprintf
#define vsniprintf vsnprintf

namespace core {

constexpr int kRead = 0, kWrite = 1, kAppend = 2, kReadWrite = 3;

class File {
 public:
  File() = default;
  File(const File& o) { *this = o; }
  File& operator=(const File& o);
  ~File() { close(); }
  bool open(const char* path, int mode);
  bool open(File* dir, uint32_t index, int mode);
  bool openNext(File* dir, int mode);
  bool close();
  bool isOpen() const { return fp_ != nullptr || dir_; }
  bool isDir() const { return dir_; }
  size_t getName(char* out, size_t n) const;
  uint32_t dirIndex() const { return index_; }
  uint64_t fileSize() const { return size_; }
  int read(void* buf, size_t n);
  int read();
  size_t write(const void* buf, size_t n);
  size_t write(uint8_t b) { return write(&b, 1); }
  bool seekSet(uint64_t pos);
  uint64_t curPosition() const { return pos_; }
  int available() const { return dir_ || pos_ >= size_ ? 0 : (int)(size_ - pos_); }
  void rewind() { seekSet(0); next_ = 0; }
  bool isContiguous() const { return true; }
  bool contiguousRange(uint32_t* first, uint32_t* last) const { *first = *last = 0; return false; }
  bool getModifyDateTime(uint16_t* date, uint16_t* time) const;   // FAT encoding of the file's mtime

 private:
  bool open_full(const std::string& card_path, int mode, uint32_t index);
  std::string path_;                 // the card's path
  FILE* fp_ = nullptr;
  bool dir_ = false;
  std::vector<std::string> names_;   // a directory's entries ('.' and '..' left out), sorted
  uint32_t next_ = 0, index_ = 0;
  uint64_t size_ = 0, pos_ = 0;
  int mode_ = kRead;
};

class Fs {
 public:
  bool begin();
  bool mkdir(const char* path);
  bool rename(const char* from, const char* to);
  bool remove(const char* path);
  bool exists(const char* path);
  uint64_t tree_hash(const char* path, uint32_t* entries, uint32_t* hidden);
  const char* type_name() const { return "posix"; }
};

void host_set_card(const char* dir);                      // the folder standing in for the card
void host_fake_time(bool on, uint64_t us);                // tests: now_us / now_ms read us (off: the real clock)
void host_set_log_sink(void (*sink)(const char* line, int n));   // default: stderr
std::string host_path(const char* card_path);              // card path -> Mac path

}  // namespace core
