// The sample RAM's allocator (docs/psram.md): one contiguous block per
// resident sample in an arena (the Teensy: PSRAM, after a reserve for other
// uses; the simulator: any buffer), so a voice reads a sample as a plain
// int16 array. Shared by the simulator (its tests, through libsampler) and the
// Teensy (libraries/Sampler).
//
// Blocks are kept in address order. Alloc finds room for a new block: a free
// hole if one is big enough (the smallest that fits), else the window of
// neighbouring blocks to evict that is the least recently used (the window
// whose most recent use is oldest; fewer bytes evicted on a tie), never one
// that is pinned (a voice plays it) - so fragmentation costs an eviction of
// a neighbour, never a compaction (moving megabytes of PSRAM would block the
// loop for 100s of ms). No memory is touched here: the caller fills the block.
#pragma once
#include <stdint.h>
#include <string.h>

namespace rytm {

class SampleRam {
 public:
  static const int kMax = 160;   // blocks (the pool's 127 slots and room)

  struct Block {
    uint32_t key;    // the caller's (the Teensy: folder << 16 | file)
    uint32_t off, bytes;
    uint32_t used;   // the last Touch's tick
    uint16_t pins;
    uint8_t user;    // the caller's state (the Teensy: loading / ready)
  };

  // bytes of arena at base (base may be null: offsets only), blocks aligned to
  // `align` bytes (a power of two).
  void Init(uint8_t* base, uint32_t bytes, uint32_t align = 4096) {
    base_ = base;
    size_ = bytes & ~(align - 1);
    align_ = align;
    n_ = 0;
    next_id_ = 0;
    evictions_ = 0;
  }

  int Find(uint32_t key) const {
    for (int i = 0; i < n_; ++i)
      if (b_[i].key == key) return id(i);
    return -1;
  }

  // A block of `bytes` for key at tick `now`; -1 if no window can be freed
  // (pinned blocks in the way, or larger than the arena). Evicts as needed:
  // the evicted keys go to evicted[] (up to max_ev; the count to *nev) so the
  // caller can forget them. may_evict false: a free hole or nothing (a
  // prefetch).
  int Alloc(uint32_t key, uint32_t bytes, uint32_t now, uint32_t* evicted = nullptr, int max_ev = 0,
            int* nev = nullptr, bool may_evict = true) {
    if (nev) *nev = 0;
    uint32_t need = (bytes + align_ - 1) & ~(align_ - 1);
    if (!need || need > size_) return -1;
    bool full = n_ >= kMax;   // no block left: evict, even if a hole fits
    if (full && !may_evict) return -1;
    // 1. the smallest hole that fits
    uint32_t best_off = 0, best_hole = 0xffffffffu;
    bool found = false;
    for (int i = 0; i <= n_ && !full; ++i) {
      uint32_t s = i ? b_[i - 1].off + b_[i - 1].bytes : 0;
      uint32_t e = i < n_ ? b_[i].off : size_;
      if (e - s >= need && e - s < best_hole) {
        best_hole = e - s;
        best_off = s;
        found = true;
      }
    }
    if (!found && !may_evict) return -1;
    if (!found) {
      // 2. a window [s, s + need) starting at the arena's start, a block's end
      // or a block's start: evict what overlaps it
      uint32_t best_age = 0xffffffffu, best_bytes = 0xffffffffu;
      int best_first = -1, best_last = -1;
      for (int c = 0; c <= 2 * n_; ++c) {
        uint32_t s;
        if (c == 0) s = 0;
        else if (c <= n_) s = b_[c - 1].off + b_[c - 1].bytes;
        else s = b_[c - n_ - 1].off;
        if (s + need > size_ || s + need < s) continue;
        uint32_t newest = 0, ev = 0;
        int first = -1, last = -1;
        bool ok = true;
        for (int i = 0; i < n_; ++i) {
          const Block& x = b_[i];
          if (x.off + x.bytes <= s || x.off >= s + need) continue;
          if (x.pins) {
            ok = false;
            break;
          }
          if (first < 0) first = i;
          last = i;
          ev += x.bytes;
          if (x.used > newest) newest = x.used;
        }
        if (!ok || first < 0) continue;
        uint32_t age = newest;   // the window's most recent use: older is better
        if (age < best_age || (age == best_age && ev < best_bytes)) {
          best_age = age;
          best_bytes = ev;
          best_first = first;
          best_last = last;
          best_off = s;
          found = true;
        }
      }
      if (!found) return -1;
      for (int i = best_last; i >= best_first; --i) {
        if (nev && *nev < max_ev && evicted) evicted[(*nev)++] = b_[i].key;
        ++evictions_;
        Remove(i);
      }
    }
    // insert in address order
    int at = 0;
    while (at < n_ && b_[at].off < best_off) ++at;
    memmove(&b_[at + 1], &b_[at], (n_ - at) * sizeof(Block));
    memmove(&ids_[at + 1], &ids_[at], (n_ - at) * sizeof(ids_[0]));
    ++n_;
    Block& x = b_[at];
    x.key = key;
    x.off = best_off;
    x.bytes = need;
    x.used = now;
    x.pins = 0;
    x.user = 0;
    ids_[at] = next_id_;
    next_id_ = next_id_ == 0x7fffffff ? 0 : next_id_ + 1;
    return ids_[at];
  }

  void Free(int id) {
    int i = index(id);
    if (i >= 0) Remove(i);
  }
  void Touch(int id, uint32_t now) {
    int i = index(id);
    if (i >= 0) b_[i].used = now;
  }
  void Pin(int id) {
    int i = index(id);
    if (i >= 0) ++b_[i].pins;
  }
  void Unpin(int id) {
    int i = index(id);
    if (i >= 0 && b_[i].pins) --b_[i].pins;
  }
  Block* Get(int id) {
    int i = index(id);
    return i >= 0 ? &b_[i] : nullptr;
  }
  uint8_t* Data(int id) {
    int i = index(id);
    return i >= 0 && base_ ? base_ + b_[i].off : nullptr;
  }

  int count() const { return n_; }
  const Block& at(int i) const { return b_[i]; }   // address order
  uint32_t size() const { return size_; }
  uint32_t used() const {
    uint32_t u = 0;
    for (int i = 0; i < n_; ++i) u += b_[i].bytes;
    return u;
  }
  uint32_t largest_hole() const {
    uint32_t best = 0;
    for (int i = 0; i <= n_; ++i) {
      uint32_t s = i ? b_[i - 1].off + b_[i - 1].bytes : 0;
      uint32_t e = i < n_ ? b_[i].off : size_;
      if (e - s > best) best = e - s;
    }
    return best;
  }
  uint32_t evictions() const { return evictions_; }

 private:
  int id(int i) const { return ids_[i]; }
  int index(int id) const {
    if (id < 0) return -1;
    for (int i = 0; i < n_; ++i)
      if (ids_[i] == id) return i;
    return -1;
  }
  void Remove(int i) {
    memmove(&b_[i], &b_[i + 1], (n_ - i - 1) * sizeof(Block));
    memmove(&ids_[i], &ids_[i + 1], (n_ - i - 1) * sizeof(ids_[0]));
    --n_;
  }

  uint8_t* base_ = nullptr;
  uint32_t size_ = 0, align_ = 4096;
  Block b_[kMax];
  int32_t ids_[kMax];   // stable handles: a block's index moves as others come and go
  int32_t next_id_ = 0;
  int n_ = 0;
  uint32_t evictions_ = 0;
};

}  // namespace rytm
