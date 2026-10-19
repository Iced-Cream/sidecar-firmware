// core/rec/mini_rec.cpp - see mini_rec.h. Everything is cold (program flash on the Teensy) but the producer
// (in_block, room, put): the USB host interrupt's few hundred bytes of copies.
#include "rec/mini_rec.h"

#include <string.h>

#include "platform.h"
#include "rec/rec_wav.h"

namespace rec {

namespace {
CORE_COLD uint32_t pow2_floor(uint32_t v) {
  uint32_t p = 1;
  while (p * 2 <= v && p < 0x80000000u) p *= 2;
  return v ? p : 0;
}
const uint64_t kPreallocMax = 256ull << 20;   // a file's reservation at its start (FAT32: its FAT entries written then)
const uint64_t kFreeMargin = 16ull << 20;     // left on the card: a take stops before it
}  // namespace

// ---- control

CORE_COLD void MiniRecorder::init(void* mem, uint32_t bytes, uint32_t block, unsigned max_samples, bool psram,
                                  const MiniBackend* b) {
  b_ = b;
  mem_ = (uint8_t*)mem;
  mem_bytes_ = mem ? bytes : 0;
  block_max_ = pow2_floor(block < 512 ? 512 : block);
  max_samples_ = max_samples;
  psram_ = psram;
  for (int i = 0; i < kMiniSources; ++i) out_[i].h = -1;
  if (!mem_ || mem_bytes_ < 2 * 512 || !layout(mask_, nullptr, 0)) mem_ = nullptr, mem_bytes_ = 0;
}

// the selection's rings: a power of 2 each, the block a quarter of one at most
CORE_COLD bool MiniRecorder::layout(uint32_t mask, char* err, unsigned errlen) {
  unsigned n = files(mask);
  if (!n) {
    cap_ = blk_ = 0;
    return true;
  }
  uint32_t cap = pow2_floor(mem_bytes_ / n), blk = block_max_;
  while (blk > 512 && blk * 4 > cap) blk /= 2;
  if (cap < 2 * blk || blk < 512) {
    if (err) sniprintf(err, errlen, CORE_STR("too many channels for the memory (%u files)"), n);
    return false;
  }
  cap_ = cap, blk_ = blk;
  return true;
}

CORE_COLD bool MiniRecorder::configure(uint32_t mask, unsigned bits, char* err, unsigned errlen) {
  if (state_ != kIdle) {
    if (err) sniprintf(err, errlen, CORE_STR("busy: stop first"));
    return false;
  }
  if (bits != 16 && bits != 24) {
    if (err) sniprintf(err, errlen, CORE_STR("bits: 16 or 24"));
    return false;
  }
  if (mask & ~kMiniMask) {
    if (err) sniprintf(err, errlen, CORE_STR("the Mini records MASTER and IN 1..12 (ids 0..12) only"));
    return false;
  }
  if (samples_per_frame(mask) > max_samples_) {
    if (err)
      sniprintf(err, errlen, max_samples_ <= 2 ? CORE_STR("stems need PSRAM: stereo (MASTER) only on this Teensy")
                                              : CORE_STR("too many channels for the memory"));
    return false;
  }
  if (mem_ && !layout(mask, err, errlen)) return false;
  mask_ = mask;
  bits_ = bits;
  return true;
}

CORE_COLD bool MiniRecorder::set_prefix(const char* p) {
  if (!valid_prefix(p)) return false;
  memcpy(prefix_, p, strlen(p) + 1);
  return true;
}

CORE_COLD void MiniRecorder::refresh_space() {
  if (state_ != kIdle || !b_ || !b_->space) return;
  space_known_ = b_->space(&free_, &total_);
}

CORE_COLD bool MiniRecorder::begin(bool arm, char* err, unsigned errlen, bool rearm) {
  if (!mem_) {
    if (err) sniprintf(err, errlen, CORE_STR("off: no card or no memory"));
    return false;
  }
  if (!mask_) {
    if (err) sniprintf(err, errlen, CORE_STR("no channel selected"));
    return false;
  }
  if (state_ == kRecording || state_ == kEnded) {
    if (err) sniprintf(err, errlen, CORE_STR("a take is running"));
    return false;
  }
  if (state_ == kArmed) {   // armed already: start now (manual), or stay armed
    arm_mode_ = arm;
    if (!arm) go_ = 1;
    return true;
  }
  if (!space_known_) refresh_space();   // once after boot: the card's free space (one FAT / bitmap scan)
  if (space_known_ && free_ < kFreeMargin + (2u << 20)) {   // room for a few seconds at least
    if (err) sniprintf(err, errlen, CORE_STR("card full"));
    return false;
  }
  layout(mask_, nullptr, 0);
  nout_ = 0;
  for (int i = 0; i < kMiniSources; ++i) {
    if (!(mask_ & (1u << i))) continue;
    Out& o = out_[nout_++];
    o.h = -1;
    o.ring = mem_ + (size_t)(nout_ - 1) * cap_;
    o.wr = o.rd = 0;
    o.data = 0;
    o.src = (uint8_t)i;
    o.ch = source(i).ch;
    o.width = source(i).width;
    o.fbytes = (uint8_t)(o.width * (bits_ / 8));
  }
  gap_ = 0;
  frames_ = 0;
  if (!rearm) {   // re-armed (arm mode, after a take): the last take's figures stay shown until the next starts
    drops_ = dropped_frames_ = 0;
    last_drops_ = 0;
    bytes_ = write_us_ = 0;
    worst_us_ = writes_ = busy_waits_ = ring_max_ = 0;
    take_[0] = 0;
  }
  error_[0] = 0;
  open_ = stopping_ = false;
  phase_ = next_ = sync_next_ = 0;
  last_in_us_ = b_->now_us();
  last_in_frames_ = in_frames_;
  arm_mode_ = arm;
  go_ = arm ? 0 : 1;
  __atomic_store_n(&state_, (int)kArmed, __ATOMIC_RELEASE);   // the producer starts at its next block once go_
  return true;
}

CORE_COLD bool MiniRecorder::start(char* err, unsigned errlen) { return begin(false, err, errlen); }
CORE_COLD bool MiniRecorder::arm(char* err, unsigned errlen) { return begin(true, err, errlen); }

CORE_COLD void MiniRecorder::stop() {
  arm_mode_ = false;
  go_ = 0;
  if (!cas(kArmed, kIdle)) cas(kRecording, kEnded);
}

CORE_COLD void MiniRecorder::transport(uint8_t s) { transport_gate(s, arm_mode_, &go_); }

CORE_COLD void MiniRecorder::end_take(const char* why) {   // stopped with its data kept (the files closed as on stop)
  sniprintf(error_, sizeof error_, CORE_STR("%s"), why);
  if (b_->log) {
    char l[120];
    sniprintf(l, sizeof l, CORE_STR("rec: %s: %s - stopped"), take_[0] ? take_ : "-", why);
    b_->log(l);
  }
  arm_mode_ = false;
  go_ = 0;
  if (!cas(kArmed, kIdle)) cas(kRecording, kEnded);
}

CORE_COLD void MiniRecorder::fail(const char* why) {   // the card failed: what is left in the rings is dropped
  end_take(why);
  stopping_ = true;
}

// ---- producer (the USB host interrupt): hot, small

bool MiniRecorder::room(uint32_t frames) const {
  for (int i = 0; i < nout_; ++i) {
    const Out& o = out_[i];
    if (cap_ - (o.wr - __atomic_load_n(&o.rd, __ATOMIC_ACQUIRE)) < frames * o.fbytes) return false;
  }
  return true;
}

void MiniRecorder::put(const int32_t* f, unsigned n, unsigned stride) {   // f null: silence
  const uint32_t m = cap_ - 1;
  const bool b24 = bits_ == 24;
  for (int i = 0; i < nout_; ++i) {
    Out& o = out_[i];
    uint8_t* r = o.ring;
    uint32_t w = o.wr;
    for (unsigned k = 0; k < n; ++k)
      for (int c = 0; c < o.width; ++c) {
        int32_t v = f ? f[k * stride + o.ch + c] : 0;
        if (b24) r[w & m] = (uint8_t)(v >> 8), ++w;
        r[w & m] = (uint8_t)(v >> 16), ++w;
        r[w & m] = (uint8_t)(v >> 24), ++w;
      }
    __atomic_store_n(&o.wr, w, __ATOMIC_RELEASE);
  }
  frames_ = frames_ + n;
}

void MiniRecorder::in_block(const int32_t* f, unsigned n, unsigned stride) {
  int st = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
  if ((st != kArmed && st != kRecording) || stride < (unsigned)kInChannels || n == 0) return;
  in_frames_ = in_frames_ + n;
  for (int i = 0; i < nout_; ++i) {   // the meters of the selected sources
    const Out& o = out_[i];
    uint32_t pk = peak_[o.src];
    for (unsigned k = 0; k < n; ++k)
      for (int c = 0; c < o.width; ++c) {
        int32_t v = f[k * stride + o.ch + c];
        uint32_t a = (uint32_t)(v < 0 ? -(v + 1) : v) >> 8;
        if (a > pk) pk = a;
      }
    peak_[o.src] = pk;
  }
  if (st == kArmed) {
    if (go_) cas(kArmed, kRecording);   // the take starts with the next block
    return;
  }
  if (gap_) {   // a drop's frames back as silence first, a few a call (the card caught up)
    uint32_t z = gap_ < kGapStep ? gap_ : kGapStep;
    if (room(z)) put(nullptr, z, 0), gap_ = gap_ - z;
  }
  if (gap_ || !room(n)) {   // the card is behind: this block is dropped (silence in its place later)
    gap_ = gap_ + n;
    drops_ = drops_ + 1;
    dropped_frames_ = dropped_frames_ + n;
    return;
  }
  put(f, n, stride);
}

// ---- consumer (the main loop)

namespace {
struct TakeScan { const char* prefix; int next; };
CORE_COLD void take_seen(void* ctx, const char* name, bool is_dir) {
  TakeScan* t = (TakeScan*)ctx;
  int v = is_dir ? take_number(name, t->prefix) : -1;
  if (v >= t->next) t->next = v + 1;
}
}  // namespace

// one step of a take's opening a call: the folder and its number, then one file each
CORE_COLD bool MiniRecorder::open_step() {
  char path[96];
  if (phase_ == 0) {
    if (!b_->mkdir("/Recordings")) return fail("no /Recordings folder"), true;
    TakeScan t = {prefix_, 1};
    b_->list("/Recordings", take_seen, &t);
    take_name(take_, sizeof take_, prefix_, t.next);
    take_dir(path, sizeof path, take_);
    if (!b_->mkdir(path)) return fail("cannot make the take's folder"), true;
    if (b_->save) b_->save(kMarker, take_);
    bytes_ = write_us_ = 0;
    worst_us_ = writes_ = busy_waits_ = ring_max_ = 0;
    free_at_start_ = space_known_ ? free_ : 0;
    phase_ = 1;
    return true;
  }
  int i = phase_ - 1;
  Out& o = out_[i];
  unsigned sum = 0;
  for (int k = 0; k < nout_; ++k) sum += out_[k].fbytes;
  uint64_t pre = 0;   // the card's free space shared by the files' rates, at most kPreallocMax each
  if (free_at_start_ > kFreeMargin * 2) pre = (free_at_start_ - kFreeMargin * 2) / sum * o.fbytes;
  if (pre > kPreallocMax) pre = kPreallocMax;
  pre &= ~(uint64_t)((1u << 20) - 1);
  file_path(path, sizeof path, take_, o.src);
  o.h = b_->open(path, pre);
  if (o.h < 0) return fail("cannot create a file (card full?)"), true;
  uint8_t h[recwav::kHeader];
  recwav::build(h, o.width, bits_, recwav::placeholder(o.fbytes));
  if (!b_->write(o.h, h, sizeof h)) return fail("write failed (card full?)"), true;
  if (++phase_ > nout_) {
    open_ = true;
    last_sync_us_ = b_->now_us();
    if (b_->log) {
      char l[160], ids[48];
      list_ids(mask_, ids, sizeof ids);
      sniprintf(l, sizeof l, CORE_STR("rec: %s started: %d files, %u-bit, channels %s, %lu KB/s, ring %lu KB a file (%lu ms), "
                                      "writes of %lu KB%s"), take_, nout_, bits_, ids,
                (unsigned long)(bytes_per_second(mask_, bits_) >> 10), (unsigned long)(cap_ >> 10),
                (unsigned long)ring_ms(), (unsigned long)(blk_ >> 10),
                arm_mode_ ? ", armed by PLAY" : "");
      b_->log(l);
    }
  }
  return true;
}

CORE_COLD bool MiniRecorder::write_ring(Out& o, uint32_t n) {   // n bytes from rd (contiguous in the ring)
  uint32_t t = b_->now_us();
  bool ok = b_->write(o.h, o.ring + (o.rd & (cap_ - 1)), n);
  uint32_t us = b_->now_us() - t;
  if (us > worst_us_) worst_us_ = us;
  write_us_ += us;
  writes_++;
  bytes_ += n;
  o.data += n;
  __atomic_store_n(&o.rd, o.rd + n, __ATOMIC_RELEASE);
  if (!ok) fail("write failed (card full or removed?)");
  return ok;
}

// the end of a take, one step a call: the partial blocks, then each file's header and close
CORE_COLD bool MiniRecorder::finish_step() {
  if (stopping_)
    for (int i = 0; i < nout_; ++i) __atomic_store_n(&out_[i].rd, __atomic_load_n(&out_[i].wr, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
  if (open_ && !stopping_)
    for (int i = 0; i < nout_; ++i) {
      Out& o = out_[i];
      uint32_t avail = __atomic_load_n(&o.wr, __ATOMIC_ACQUIRE) - o.rd;
      if (avail) {   // under a block, contiguous (rd sits on a block boundary)
        write_ring(o, avail);
        return true;
      }
    }
  for (int i = 0; i < nout_; ++i) {
    Out& o = out_[i];
    if (o.h < 0) continue;
    if ((o.data & 1) && !stopping_) {
      uint8_t z = 0;
      b_->write(o.h, &z, 1);   // RIFF's pad byte
    }
    uint8_t h[recwav::kHeader];
    recwav::build(h, o.width, bits_, (uint32_t)o.data);
    bool ok = b_->header(o.h, h, sizeof h);
    ok = b_->close(o.h, recwav::kHeader + o.data + (o.data & 1)) && ok;
    o.h = -1;
    if (!ok && !error_[0]) sniprintf(error_, sizeof error_, CORE_STR("header write failed"));
    return true;
  }
  // all closed
  bool had = phase_ > 0;
  if (had && b_->remove) b_->remove(kMarker);
  if (had && b_->log) {
    uint32_t kbps = write_us_ ? (uint32_t)(bytes_ * 1000000 / write_us_ >> 10) : 0;
    uint32_t fr = frames_;
    char l[220];
    sniprintf(l, sizeof l, CORE_STR("rec: %s stopped: %lu frames (%lu.%01lu s), %lu KB in %lu writes, card %lu KB/s, worst write %lu us, "
                                    "ring max %lu%%, card busy %lu polls, dropped %lu blocks (%lu frames)"), take_[0] ? take_ : "-",
              (unsigned long)fr, (unsigned long)(fr / 48000), (unsigned long)(fr % 48000 * 10 / 48000),
              (unsigned long)(bytes_ >> 10), (unsigned long)writes_, (unsigned long)kbps, (unsigned long)worst_us_,
              (unsigned long)ring_max_, (unsigned long)busy_waits_, (unsigned long)drops_, (unsigned long)dropped_frames_);
    b_->log(l);
  }
  open_ = stopping_ = false;
  phase_ = 0;
  if (free_at_start_) free_ = free_at_start_ > bytes_ ? free_at_start_ - bytes_ : 0;
  if (arm_mode_ && !error_[0]) {   // the next PLAY makes the next take
    char e[8];
    __atomic_store_n(&state_, (int)kIdle, __ATOMIC_RELEASE);
    uint8_t g = go_;
    begin(true, e, sizeof e, true);
    go_ = g;   // a PLAY that came while this take was closing starts the next at once
  } else {
    arm_mode_ = false;
    __atomic_store_n(&state_, (int)kIdle, __ATOMIC_RELEASE);
  }
  return true;
}

CORE_COLD bool MiniRecorder::poll() {
  if (!b_ || !mem_) return false;
  uint32_t now = b_->now_us();
  int st = state_;
  if (st == kRecording && !go_) cas(kRecording, kEnded), st = state_;
  if (st != kRecording && st != kEnded) {
    last_in_us_ = now;
    return false;
  }
  if (in_frames_ != last_in_frames_) last_in_frames_ = in_frames_, last_in_us_ = now;
  else if (st == kRecording && now - last_in_us_ > kStallUs) end_take("the Overbridge stream stopped"), st = state_;
  bool card_busy = b_->busy && b_->busy();   // the card programs the last write: come back (the ring holds the stream)
  if (card_busy) busy_waits_++;
  if (!open_ && !stopping_ && phase_ <= nout_) return card_busy ? false : open_step();
  if (drops_ != last_drops_ && now - last_drop_log_us_ > 1000000u && b_->log) {
    char l[140];
    sniprintf(l, sizeof l, CORE_STR("rec: %s: the card is behind: %lu blocks dropped (%lu frames written as silence)"), take_,
              (unsigned long)drops_, (unsigned long)dropped_frames_);
    b_->log(l);
    last_drops_ = drops_;
    last_drop_log_us_ = now;
  }
  if (st == kRecording && !stopping_) {   // the limits: the card's free space, FAT32's / RIFF's 4 GB
    uint64_t widest = 0;
    for (int i = 0; i < nout_; ++i)
      if (out_[i].data > widest) widest = out_[i].data;
    if (free_at_start_ && bytes_ + kFreeMargin + 4 * (uint64_t)blk_ * nout_ >= free_at_start_) end_take("card full");
    else if (widest + 4 * (uint64_t)blk_ >= kMaxData) end_take("4 GB file limit");
    st = state_;
  }
  for (int i = 0; i < nout_; ++i) {   // the high-water mark
    uint32_t used = (uint32_t)((uint64_t)(__atomic_load_n(&out_[i].wr, __ATOMIC_ACQUIRE) - out_[i].rd) * 100 / cap_);
    if (used > ring_max_) ring_max_ = used;
  }
  if (card_busy) return false;
  if (!stopping_)
    for (int k = 0; k < nout_; ++k) {   // one whole block a call, the files in turn
      int i = (next_ + k) % nout_;
      Out& o = out_[i];
      if (__atomic_load_n(&o.wr, __ATOMIC_ACQUIRE) - o.rd >= blk_) {
        next_ = i + 1;
        write_ring(o, blk_);
        return true;
      }
    }
  if (st == kEnded) return finish_step();
  if (now - last_sync_us_ > 2000000u && nout_) {   // every 2 s one file's size reaches the directory (readable to there)
    last_sync_us_ = now;
    b_->sync(out_[sync_next_ % nout_].h);
    sync_next_++;
    return true;
  }
  return false;
}

CORE_COLD uint32_t MiniRecorder::ring_ms() const {   // the shortest file ring's length at its rate
  unsigned widest = 0;
  for (int i = 0; i < kMiniSources; ++i)
    if ((mask_ & (1u << i)) && source(i).width > widest) widest = source(i).width;
  return widest ? (uint32_t)((uint64_t)cap_ * 1000 / (48000u * widest * (bits_ / 8))) : 0;
}

CORE_COLD void MiniRecorder::status(MiniStatus* s, bool take_peaks) {
  memset(s, 0, sizeof *s);
  s->state = state_;
  s->arm_mode = arm_mode_;
  s->ready = mem_ != nullptr;
  s->psram = psram_;
  s->bits = bits_;
  s->max_samples = max_samples_;
  s->mask = mask_;
  memcpy(s->take, take_, sizeof s->take);
  memcpy(s->prefix, prefix_, sizeof s->prefix);
  memcpy(s->error, error_, sizeof s->error);
  s->frames = frames_;
  for (int i = 0; i < kMiniSources; ++i) {
    s->peak[i] = peak_[i];
    if (take_peaks) peak_[i] = 0;
  }
  s->bytes = bytes_;
  s->drops = drops_;
  s->dropped_frames = dropped_frames_;
  s->write_kbps = write_us_ ? (uint32_t)(bytes_ * 1000000 / write_us_ >> 10) : 0;
  s->worst_write_us = worst_us_;
  s->writes = writes_;
  s->busy_waits = busy_waits_;
  s->ring_bytes = cap_;
  s->block = blk_;
  s->ring_ms = ring_ms();
  s->ring_max_pct = ring_max_;
  s->need_kbps = bytes_per_second(mask_, bits_) >> 10;
  uint64_t used = state_ == kRecording || state_ == kEnded ? bytes_ : 0;
  uint64_t fr = state_ != kIdle && free_at_start_ ? free_at_start_ : free_;
  s->free_bytes = fr > used ? fr - used : 0;
  s->total_bytes = total_;
}

namespace {
struct FixScan { const MiniBackend* b; const char* take; int fixed; };
CORE_COLD void fix_one(void* ctx, const char* name, bool is_dir) {
  FixScan* x = (FixScan*)ctx;
  if (is_dir || !is_wav_name(name)) return;
  char fp[120];
  sniprintf(fp, sizeof fp, CORE_STR("/Recordings/%s/%s"), x->take, name);
  uint8_t h[recwav::kHeader];
  uint64_t size = 0;
  if (!x->b->read_at(fp, h, sizeof h, &size) || size < recwav::kHeader) return;
  if (recwav::fix(h, size) && x->b->patch(fp, 0, h, sizeof h)) {
    x->fixed++;
    if (x->b->log) {
      char m[180];
      sniprintf(m, sizeof m, CORE_STR("rec: %s: sizes fixed (%lu KB: a take cut short)"), fp, (unsigned long)(size >> 10));
      x->b->log(m);
    }
  }
}
}  // namespace

CORE_COLD int MiniRecorder::fix_pending() {
  if (!b_ || !b_->load) return 0;
  char take[40];
  int n = b_->load(kMarker, take, sizeof take);
  if (n <= 0) return 0;
  take[n < (int)sizeof take ? n : (int)sizeof take - 1] = 0;
  for (char* c = take; *c; ++c)
    if (*c == '\n' || *c == '\r') *c = 0;
  FixScan x = {b_, take, 0};
  if (valid_prefix(take)) {
    char dir[64];
    take_dir(dir, sizeof dir, take);
    b_->list(dir, fix_one, &x);
  }
  b_->remove(kMarker);
  return x.fixed;
}

}  // namespace rec
