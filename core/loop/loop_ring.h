// core/loop/loop_ring.h - the looper's MAIN ring (spec-050 3.3): the Rytm's MAIN L / R as it arrives, always, int32 (24-bit
// full scale << 8) L R interleaved, `cap` frames (the Pi: 4,194,304 = 87.38 s). Header-only, lock-free, one writer (core 0,
// the Overbridge IN completion) and readers on core 3 (the grabs' copies, the playback's frames): the write count is
// published with release, read with acquire. A frame has an absolute index (frames written before it); the map host render
// frame <-> index is the minimum arrival offset over the last 24 blocks (seq::Follower's way: the USB path's jitter only
// ever delays), so `frame_at(h)` gives the frame that arrived at host frame h, jitter-free. A lost block is written as
// silence so the map stays linear; the stretch is remembered for gap_in (one gap, the latest).
#pragma once
#include <stdint.h>
#include <string.h>

namespace core {
namespace loop {

class Ring {
 public:
  static constexpr int kWindow = 24;
  void init(int32_t* mem, uint32_t frames) {
    mem_ = mem, cap_ = frames, w_ = 0, n_ = 0, k_ = 0, have_ = false, gap0_ = gap1_ = 0;
    off_ = 0;
  }
  bool ready() const { return mem_ && cap_; }
  uint32_t capacity() const { return cap_; }
  int32_t* data() const { return mem_; }
  // the writer: n frames (L R interleaved) that arrived with the host's render frame `arrival` for the last of them,
  // preceded by `lost` frames that never came (written as silence)
  void write(const int32_t* lr, int n, uint64_t arrival, unsigned lost) {
    uint64_t w = w_;
    if (lost) {
      gap0_ = w, gap1_ = w + lost;
      for (unsigned i = 0; i < lost; ++i) put(w + i, 0, 0);
      w += lost;
    }
    // the block's offset: host frame - index, the same for each of its frames (arrival of frame i = arrival - (n - 1 - i))
    int64_t d = (int64_t)arrival - (int64_t)(n - 1) - (int64_t)w;
    win_[k_] = d;
    k_ = (k_ + 1) % kWindow;
    if (n_ < kWindow) ++n_;
    int64_t m = win_[0];
    for (int i = 1; i < n_; ++i)
      if (win_[i] < m) m = win_[i];
    // a step back of the whole window (a new link) is taken at once; the minimum follows a faster path down only
    off_ = m, have_ = true;
    for (int i = 0; i < n; ++i) put(w + i, lr[2 * i], lr[2 * i + 1]);
    __atomic_store_n(&w_, w + n, __ATOMIC_RELEASE);
  }
  uint64_t written() const { return __atomic_load_n(&w_, __ATOMIC_ACQUIRE); }
  // the offset a reader uses (host = index + offset); false until a block came
  bool offset(int64_t* o) const {
    *o = off_;
    return have_;
  }
  // the ring index of the frame that arrived at host frame h; false when it is not (yet) in the ring
  bool frame_at(uint64_t h, uint32_t* idx) const {
    uint64_t k;
    if (!index_at(h, &k)) return false;
    *idx = (uint32_t)(k % cap_);
    return true;
  }
  // the absolute index (frames written before it) of host frame h's frame, if it is in the ring
  bool index_at(uint64_t h, uint64_t* k) const {
    if (!have_) return false;
    int64_t i = (int64_t)h - off_;
    uint64_t w = written();
    if (i < 0 || (uint64_t)i >= w || w - (uint64_t)i > cap_) return false;
    *k = (uint64_t)i;
    return true;
  }
  uint64_t host_of(uint64_t k) const { return (uint64_t)((int64_t)k + off_); }
  // n frames from the frame at host frame `from`, into out (L R interleaved); the count copied (0: not in the ring, or
  // overwritten while copying)
  uint32_t copy(uint64_t from, uint32_t n, int32_t* out) const {
    uint64_t k;
    if (!index_at(from, &k)) return 0;
    return copy_idx(k, n, out);
  }
  uint32_t copy_idx(uint64_t k, uint32_t n, int32_t* out) const {
    uint64_t w = written();
    if (k >= w || w - k > cap_ || n == 0) return 0;
    if (k + n > w) n = (uint32_t)(w - k);
    uint32_t i = (uint32_t)(k % cap_), first = cap_ - i < n ? cap_ - i : n;
    memcpy(out, mem_ + 2 * (size_t)i, 8u * first);
    if (first < n) memcpy(out + 2 * (size_t)first, mem_, 8u * (n - first));
    return written() - k > cap_ ? 0 : n;   // the writer lapped the first frame meanwhile: not a copy
  }
  // a gap (frames that never arrived) in the n frames from host frame `from`
  bool gap_in(uint64_t from, uint32_t n) const {
    if (gap1_ <= gap0_) return false;
    uint64_t k0 = (int64_t)from - off_ < 0 ? 0 : (uint64_t)((int64_t)from - off_);
    return k0 < gap1_ && k0 + n > gap0_;
  }
  void frame(uint64_t k, int32_t* l, int32_t* r) const {   // frame k (it must be in the ring)
    uint32_t i = (uint32_t)(k % cap_);
    *l = mem_[2 * (size_t)i], *r = mem_[2 * (size_t)i + 1];
  }

 private:
  void put(uint64_t k, int32_t l, int32_t r) {
    uint32_t i = (uint32_t)(k % cap_);
    mem_[2 * (size_t)i] = l, mem_[2 * (size_t)i + 1] = r;
  }
  int32_t* mem_ = nullptr;
  uint32_t cap_ = 0;
  uint64_t w_ = 0;
  int64_t win_[kWindow];
  int n_ = 0, k_ = 0;
  int64_t off_ = 0;
  bool have_ = false;
  uint64_t gap0_ = 0, gap1_ = 0;
};

}  // namespace loop
}  // namespace core
