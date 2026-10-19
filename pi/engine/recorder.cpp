// pi/engine/recorder.cpp - see recorder.h.
#include "recorder.h"

#include <string.h>

#include "rec/rec_wav.h"
#ifdef REC_HOST   // the Mac tests
#include <stdio.h>
#else
#include "core_port.h"   // platform_pi.cpp's integer printf
#define snprintf core_snprintf
#endif

namespace rec {

// ---- control

void Recorder::init(void* mem, size_t bytes, const Backend* b) {
  b_ = b;
  size_t st = (size_t)kSources * kChunk;
  if (!mem || bytes < st + (1u << 20)) return;   // off: nothing reserved (or too little)
  stage_ = (uint8_t*)mem;
  ring_ = (int32_t*)(stage_ + st);
  ring_bytes_ = bytes - st;
  configure(mask_, bits_, nullptr, 0);
}

bool Recorder::configure(uint32_t mask, unsigned bits, char* err, unsigned errlen) {
  if (state_ != kIdle) {
    if (err) snprintf(err, errlen, "busy: stop first");
    return false;
  }
  if (bits != 16 && bits != 24) {
    if (err) snprintf(err, errlen, "bits: 16 or 24");
    return false;
  }
  mask_ = mask & ((1u << kSources) - 1);
  bits_ = bits;
  nsamp_ = samples_per_frame(mask_);
  side_on_ = false;
  for (int i = kVoice; i < kSources; ++i)
    if (mask_ & (1u << i)) side_on_ = true;
  if (ring_ && nsamp_) {
    slot_words_ = 2 + kBlockFrames * nsamp_;
    uint32_t n = (uint32_t)(ring_bytes_ / (slot_words_ * 4));
    nslots_ = 1;
    while (nslots_ * 2 <= n) nslots_ *= 2;
  } else nslots_ = 0;
  return true;
}

bool Recorder::set_prefix(const char* p) {
  if (!valid_prefix(p)) return false;
  memcpy(prefix_, p, strlen(p) + 1);
  return true;
}

bool Recorder::begin(bool arm, char* err, unsigned errlen, bool rearm) {
  if (!ring_) {
    if (err) snprintf(err, errlen, "off: no memory reserved (cmdline recbuf=MB)");
    return false;
  }
  if (!mask_) {
    if (err) snprintf(err, errlen, "no channel selected");
    return false;
  }
  if (state_ == kRecording || state_ == kEnded) {
    if (err) snprintf(err, errlen, "a take is running");
    return false;
  }
  if (state_ == kArmed) {   // armed already: start now (manual), or stay armed
    arm_mode_ = arm;
    if (!arm) go_ = 1;
    return true;
  }
  configure(mask_, bits_, nullptr, 0);
  wr_ = rd_ = gap_ = 0;
  frame_in_slot_ = zeros_ = 0;
  if (!rearm) {   // re-armed (arm mode, after a take): the counters run on, the last take's figures stay shown
    drops_ = dropped_frames_ = side_under_ = 0;
    last_drops_ = 0;
    frames_ = bytes_ = write_us_ = 0;
    worst_us_ = writes_ = ring_max_ = 0;
    take_[0] = 0;
  }
  error_[0] = 0;
  stopping_ = false;
  Side x;
  while (side_.pop(&x)) {
  }
  arm_mode_ = arm;
  go_ = arm ? 0 : 1;
  __atomic_store_n(&state_, (int)kArmed, __ATOMIC_RELEASE);   // the producer starts at its next block once go_
  return true;
}

bool Recorder::start(char* err, unsigned errlen) { return begin(false, err, errlen); }
bool Recorder::arm(char* err, unsigned errlen) { return begin(true, err, errlen); }

void Recorder::stop() {
  arm_mode_ = false;
  go_ = 0;
  if (!cas(kArmed, kIdle)) cas(kRecording, kEnded);
}

void Recorder::transport(uint8_t s) { transport_gate(s, arm_mode_, &go_); }

void Recorder::fail(const char* why) {
  snprintf(error_, sizeof error_, "%s", why);
  if (b_ && b_->log) {
    char l[120];
    snprintf(l, sizeof l, "rec: %s: %s - stopped", take_[0] ? take_ : "-", why);
    b_->log(l);
  }
  arm_mode_ = false;
  go_ = 0;
  if (!cas(kArmed, kIdle)) cas(kRecording, kEnded);
  stopping_ = true;   // what is left in the ring is dropped
}

// ---- producer

void Recorder::in_block(const int32_t (*in)[kInChannels], unsigned n) {
  int st = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
  if (st == kArmed) {
    Side x;
    while (side_.size() > 16 * kBlockFrames && side_.pop(&x)) {   // armed: the OUT side kept current, 16 blocks of lead
    }                                                           // (the OUT fill may run just after this IN block)
    for (unsigned f = 0; f < n; ++f)   // the meters of the selected IN channels while waiting
      for (int i = 0; i < kVoice; ++i)
        if (mask_ & (1u << i))
          for (int k = 0; k < source(i).width; ++k) {
            int32_t v = in[f][source(i).ch + k];
            uint32_t a = (uint32_t)(v < 0 ? -(v + 1) : v) >> 8;
            if (a > peak_[i]) peak_[i] = a;
          }
    if (!go_ || !cas(kArmed, kRecording)) return;
    return;   // the take starts with the next block: its OUT side frames are queued after this
  }
  if (st != kRecording || n == 0) return;
  if (n > kBlockFrames) n = kBlockFrames;
  Side side[kBlockFrames];
  if (side_on_)
    for (unsigned f = 0; f < n; ++f)
      if (!side_.pop(&side[f])) {
        memset(&side[f], 0, sizeof side[f]);
        side_under_ = side_under_ + 1;
      }
  uint32_t w = wr_, r = __atomic_load_n(&rd_, __ATOMIC_ACQUIRE);
  if (w - r >= nslots_) {   // the card is behind: this block is dropped (written as silence later)
    gap_ += n;
    drops_ = drops_ + 1;
    dropped_frames_ = dropped_frames_ + n;
    return;
  }
  int32_t* s = slot(w);
  s[0] = (int32_t)gap_;
  s[1] = (int32_t)n;
  gap_ = 0;
  int32_t* o = s + 2;
  for (unsigned f = 0; f < n; ++f)
    for (int i = 0; i < kSources; ++i) {
      if (!(mask_ & (1u << i))) continue;
      const Source& src = source(i);
      for (int k = 0; k < src.width; ++k) {
        int32_t v = src.side ? side[f].s[src.ch + k] : in[f][src.ch + k];
        *o++ = v;
        uint32_t a = (uint32_t)(v < 0 ? -(v + 1) : v) >> 8;
        if (a > peak_[i]) peak_[i] = a;
      }
    }
  __atomic_store_n(&wr_, w + 1, __ATOMIC_RELEASE);
}

static int32_t sat(float x) {
  float y = x * 2147483648.0f;
  return y >= 2147483647.0f ? 2147483647 : y <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)y;
}

void Recorder::out_block(const int16_t (*v)[kVoices], const float (*fx)[4], unsigned n) {
  int st = state_;
  if (!side_on_ || (st != kArmed && st != kRecording)) return;
  for (unsigned f = 0; f < n; ++f) {
    Side x;
    for (int i = 0; i < kVoices; ++i) x.s[i] = v ? (int32_t)v[f][i] * 65536 : 0;
    for (int i = 0; i < 4; ++i) x.s[kVoices + i] = fx ? sat(fx[f][i]) : 0;
    side_.push(x);   // full: only while armed with no IN stream (in_block keeps it drained)
  }
}

// ---- consumer

bool Recorder::open_take() {
  static char names[1024][64];
  static uint64_t sizes[1024];
  static bool dirs[1024];
  if (!b_->mkdir("/Recordings")) return fail("no /Recordings folder"), false;
  int n = b_->list("/Recordings", names, sizes, dirs, 1024), next = 1;
  for (int i = 0; i < n; ++i) {
    int v = take_number(names[i], prefix_);
    if (v >= next) next = v + 1;
  }
  take_name(take_, sizeof take_, prefix_, next);
  frames_ = bytes_ = write_us_ = 0;
  worst_us_ = writes_ = ring_max_ = 0;
  char path[120];
  take_dir(path, sizeof path, take_);
  if (!b_->mkdir(path)) return fail("cannot make the take's folder"), false;
  nout_ = 0;
  open_ = true;   // from here close_take() closes what was opened
  uint8_t h[recwav::kHeader];
  unsigned bps = bits_ / 8;
  for (int i = 0; i < kSources; ++i) {
    if (!(mask_ & (1u << i))) continue;
    Out& o = out_[nout_];
    file_path(path, sizeof path, take_, i);
    o.h = b_->open(path);
    o.buf = stage_ + (size_t)nout_ * kChunk;
    o.fill = 0;
    o.data = 0;
    o.width = source(i).width;
    o.src = (uint8_t)i;
    if (o.h < 0) return fail("cannot create a file"), false;
    nout_++;
    recwav::build(h, o.width, bits_, recwav::placeholder(o.width * bps));
    if (!b_->write(o.h, h, sizeof h)) return fail("write failed"), false;
  }
  last_sync_us_ = b_->now_us();
  char l[160], ids[80];
  list_ids(mask_, ids, sizeof ids);
  snprintf(l, sizeof l, "rec: %s started: %d files, %u-bit, channels %s, %u KB/s, ring %u slots (%u ms)%s", take_, nout_, bits_,
           ids, (unsigned)(bytes_per_second(mask_, bits_) >> 10), nslots_,
           (unsigned)((uint64_t)nslots_ * kBlockFrames * 1000 / recwav::kRate), arm_mode_ ? ", armed by PLAY" : "");
  if (b_->log) b_->log(l);
  return true;
}

bool Recorder::write_out(Out& o) {
  uint32_t t = b_->now_us();
  bool ok = b_->write(o.h, o.buf, o.fill);
  uint32_t us = b_->now_us() - t;
  if (us > worst_us_) worst_us_ = us;
  write_us_ += us;
  writes_++;
  bytes_ += o.fill;
  o.data += o.fill;
  o.fill = 0;
  if (!ok) fail("write failed (card full?)");
  return ok;
}

void Recorder::close_take() {
  uint8_t h[recwav::kHeader];
  for (int i = 0; i < nout_; ++i) {
    Out& o = out_[i];
    if (o.h < 0) continue;
    if (o.fill) write_out(o);
    if (o.data & 1) {
      uint8_t z = 0;
      b_->write(o.h, &z, 1);   // RIFF's pad byte
    }
    recwav::build(h, o.width, bits_, (uint32_t)o.data);
    if (!b_->pwrite(o.h, 0, h, sizeof h)) fail("header write failed");
    b_->close(o.h);
    o.h = -1;
  }
  open_ = false;
  uint32_t kbps = write_us_ ? (uint32_t)(bytes_ * 1000000 / write_us_ >> 10) : 0;
  char l[200];
  snprintf(l, sizeof l, "rec: %s stopped: %llu frames (%llu.%01llu s), %llu KB in %u writes, card %u KB/s, worst write %u ms, "
           "ring max %u%%, dropped %u blocks, OUT side short %u", take_[0] ? take_ : "-", (unsigned long long)frames_,
           (unsigned long long)(frames_ / recwav::kRate), (unsigned long long)(frames_ % recwav::kRate * 10 / recwav::kRate),
           (unsigned long long)(bytes_ >> 10), writes_, kbps, worst_us_ / 1000,
           nslots_ ? (unsigned)((uint64_t)ring_max_ * 100 / nslots_) : 0, (unsigned)drops_, (unsigned)side_under_);
  if (b_->log) b_->log(l);
}

void Recorder::poll() {
  if (!b_) return;
  uint32_t now = b_->now_us();
  if (now - last_space_us_ > 10000000u || last_space_us_ == 0) {
    last_space_us_ = now | 1;
    if (state_ != kRecording) b_->space(&free_, &total_);   // f_getfree: not between a take's writes (FatFs keeps
  }                                                          // the count in step itself; ours is estimated below)
  int st = state_;
  if (st == kRecording && !go_) cas(kRecording, kEnded), st = state_;
  if (st != kRecording && st != kEnded) return;
  if (!open_ && !stopping_ && !open_take()) return;
  if (drops_ != last_drops_ && now - last_drop_log_us_ > 1000000u && b_->log) {
    char l[120];
    snprintf(l, sizeof l, "rec: %s: the card is behind: %u blocks dropped (%u frames written as silence)", take_,
             (unsigned)drops_, (unsigned)dropped_frames_);
    b_->log(l);
    last_drops_ = drops_;
    last_drop_log_us_ = now;
  }
  for (int i = 0; i < nout_; ++i)   // one whole chunk a call
    if (out_[i].fill >= kChunk) {
      write_out(out_[i]);
      return;
    }
  if (stopping_) {   // failed: drop the rest
    __atomic_store_n(&rd_, __atomic_load_n(&wr_, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    zeros_ = 0;
  }
  // into the staging, up to a chunk
  unsigned bps = bits_ / 8;
  bool full = false;
  for (unsigned budget = 8192; budget && !full; --budget) {
    const int32_t* fr = nullptr;
    int32_t* s = nullptr;
    if (zeros_ == 0) {
      uint32_t r = rd_, w = __atomic_load_n(&wr_, __ATOMIC_ACQUIRE);
      if (r == w) break;
      if (w - r > ring_max_) ring_max_ = w - r;
      s = slot(r);
      if (frame_in_slot_ == 0 && s[0]) {
        zeros_ = (uint32_t)s[0];
        s[0] = 0;
      }
    }
    if (zeros_ == 0) fr = s + 2 + frame_in_slot_ * nsamp_;
    if (frames_ * 3 * 2 + kChunk > kMaxData) {   // the next chunk would pass FAT32's 4 GB (the widest file)
      fail("4 GB file limit");
      break;
    }
    unsigned k = 0;
    for (int i = 0; i < nout_; ++i) {
      Out& o = out_[i];
      uint8_t* d = o.buf + o.fill;
      for (int c = 0; c < o.width; ++c) {
        int32_t v = fr ? fr[k++] : 0;
        if (bps == 3) d[0] = (uint8_t)(v >> 8), d[1] = (uint8_t)(v >> 16), d[2] = (uint8_t)(v >> 24), d += 3;
        else d[0] = (uint8_t)(v >> 16), d[1] = (uint8_t)(v >> 24), d += 2;
      }
      o.fill += o.width * bps;
      if (o.fill >= kChunk) full = true;
    }
    frames_++;
    if (zeros_) zeros_--;
    else if (++frame_in_slot_ >= (uint32_t)s[1]) {
      frame_in_slot_ = 0;
      __atomic_store_n(&rd_, rd_ + 1, __ATOMIC_RELEASE);
    }
  }
  if (full) return;
  if (state_ == kEnded && zeros_ == 0 && rd_ == __atomic_load_n(&wr_, __ATOMIC_ACQUIRE)) {
    for (; flush_next_ < nout_; ++flush_next_)   // the partial chunks, one a call
      if (out_[flush_next_].fill) {
        write_out(out_[flush_next_++]);
        return;
      }
    flush_next_ = 0;
    if (open_) close_take();
    open_ = false;
    stopping_ = false;
    b_->space(&free_, &total_);
    last_space_us_ = b_->now_us() | 1;
    if (arm_mode_) {   // the next PLAY makes the next take
      char e[8];
      __atomic_store_n(&state_, (int)kIdle, __ATOMIC_RELEASE);
      uint8_t g = go_;
      begin(true, e, sizeof e, true);
      go_ = g;   // a PLAY that came while this take was closing starts the next at once
    } else __atomic_store_n(&state_, (int)kIdle, __ATOMIC_RELEASE);
    return;
  }
  if (now - last_sync_us_ > 2000000u && nout_) {   // every 2 s one file's size reaches the directory (readable to there)
    last_sync_us_ = now;
    b_->sync(out_[sync_next_ % nout_].h);
    sync_next_++;
  }
}

void Recorder::status(Status* s, bool take_peaks) {
  memset(s, 0, sizeof *s);
  s->state = state_;
  s->arm_mode = arm_mode_;
  s->ready = ring_ != nullptr;
  s->bits = bits_;
  s->mask = mask_;
  memcpy(s->take, take_, sizeof s->take);
  memcpy(s->prefix, prefix_, sizeof s->prefix);
  memcpy(s->error, error_, sizeof s->error);
  s->frames = frames_;
  for (int i = 0; i < kSources; ++i) {
    s->peak[i] = peak_[i];
    if (take_peaks) peak_[i] = 0;
  }
  s->drops = drops_;
  s->dropped_frames = dropped_frames_;
  s->side_under = side_under_;
  s->bytes = bytes_;
  s->write_kbps = write_us_ ? (uint32_t)(bytes_ * 1000000 / write_us_ >> 10) : 0;
  s->worst_write_us = worst_us_;
  s->writes = writes_;
  s->ring_slots = nslots_;
  s->ring_max_used = ring_max_;
  s->ring_used = __atomic_load_n(&wr_, __ATOMIC_ACQUIRE) - rd_;
  s->need_kbps = bytes_per_second(mask_, bits_) >> 10;
  s->ring_ms = (uint32_t)((uint64_t)nslots_ * kBlockFrames * 1000 / recwav::kRate);
  uint64_t used = state_ == kRecording || state_ == kEnded ? bytes_ : 0;   // since the last f_getfree
  s->free_bytes = free_ > used ? free_ - used : 0;
  s->total_bytes = total_;
}

int Recorder::fix_all(const char* dir) {
  static char takes[256][64], names[64][64];
  static uint64_t tsz[256], sizes[64];
  static bool tdir[256], dirs[64];
  int nt = b_->list(dir, takes, tsz, tdir, 256), fixed = 0;
  for (int t = 0; t < nt; ++t) {
    if (!tdir[t]) continue;
    char path[200];
    snprintf(path, sizeof path, "%s/%s", dir, takes[t]);
    int nf = b_->list(path, names, sizes, dirs, 64);
    for (int f = 0; f < nf; ++f) {
      if (dirs[f] || !is_wav_name(names[f])) continue;
      char fp[280];
      snprintf(fp, sizeof fp, "%s/%s", path, names[f]);
      uint8_t h[recwav::kHeader];
      if (sizes[f] < recwav::kHeader || !b_->read_at(fp, 0, h, sizeof h)) continue;
      if (recwav::fix(h, sizes[f]) && b_->patch(fp, 0, h, sizeof h)) {
        fixed++;
        if (b_->log) {
          char m[300];
          snprintf(m, sizeof m, "rec: %s: sizes fixed (%llu bytes: a take cut short)", fp, (unsigned long long)sizes[f]);
          b_->log(m);
        }
      }
    }
  }
  return fixed;
}

}  // namespace rec
