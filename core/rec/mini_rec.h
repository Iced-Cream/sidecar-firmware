// core/rec/mini_rec.h - the Sidecar Mini's recorder (the Teensy 4.1; docs/recorder-mini.md): the Overbridge IN
// stream to the card, as the Pi's multitrack recorder writes it (pi/engine/recorder.h): /Recordings/<prefix>_NNN/
// NN_NAME.wav, 512-byte headers (core/rec/rec_wav.h), 16 or 24 bit, every file starting on the same frame and the
// same length. The sources are the IN ones (rec_names.h 0..12): MASTER (stereo) on any Teensy, the 12 Overbridge
// channels as mono stems only with PSRAM (the rings need it: init's max_samples).
//
// Three sides, no locks, no allocation:
//   - producer (in_block: the USB host interrupt, after each Overbridge IN transfer): the selected samples packed
//     straight into one byte ring a file (the file's own bytes, 16- or 24-bit little endian). Room is checked for
//     every file first, so a block goes into all of them or none: a full ring drops the block, counted, and its
//     frames go in later as silence (a bounded number a call) so the files keep time and stay aligned.
//   - control (the main loop: the console, the web console, the remote page): configure, arm, start, stop.
//   - consumer (poll, the main loop between requests): at most one card operation a call - one whole block
//     (a power of 2, 8 KB without PSRAM, 32 KB with) from one ring, a file opened, a partial block flushed, a file
//     closed - and a write only while the card is not busy programming (busy()), so the loop never waits on the
//     card's own write latency: the ring covers it. The files are reserved contiguous at their start (open's
//     prealloc) so a take does not update the FAT / bitmap between blocks.
// Portable: the card is a MiniBackend (SdFat on the Teensy: teensy/voiceloop/rec_mini.h; stdio in
// core/tests/test_mini_rec.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "rec/rec_names.h"

namespace rec {

const int kMiniSources = kIn + kInChannels;   // 13: MASTER and IN 1..12
const uint32_t kMiniMask = (1u << kMiniSources) - 1;

struct MiniBackend {
  bool (*mkdir)(const char* path);                       // true if made or there
  // created (truncated) for writing, `prealloc` bytes reserved contiguous when the card can (0: none; best effort):
  // a handle 0..kMiniSources-1, -1
  int (*open)(const char* path, uint64_t prealloc);
  bool (*write)(int h, const void* p, uint32_t n);       // appended (all n, else false)
  bool (*header)(int h, const void* p, uint32_t n);      // at offset 0, then back to the end
  bool (*sync)(int h);                                   // its size to the directory
  bool (*close)(int h, uint64_t size);                   // cut to size (what was reserved past it released), closed
  bool (*busy)();                                        // the card still programs the last write (null: never)
  bool (*space)(uint64_t* free, uint64_t* total);        // may be a cached figure
  uint32_t (*now_us)();
  // the take numbering: each entry of dir; false: no such folder
  bool (*list)(const char* dir, void (*fn)(void* ctx, const char* name, bool is_dir), void* ctx);
  // the power-loss repair: a file's first n bytes and its size; n bytes written at off
  bool (*read_at)(const char* path, void* p, uint32_t n, uint64_t* size);
  bool (*patch)(const char* path, uint64_t off, const void* p, uint32_t n);
  // a small text file (the take being written, kMarker): saved, loaded (its length, -1 none), removed
  bool (*save)(const char* path, const char* text);
  int (*load)(const char* path, char* out, unsigned cap);
  bool (*remove)(const char* path);
  void (*log)(const char* line);
};

const char kMarker[] = "/Recordings/.recording";   // the take being written: a power loss leaves it, fix_pending reads it

struct MiniStatus {
  int state;               // 0 idle, 1 armed (waiting for PLAY or start), 2 recording, 3 finishing (draining, closing)
  bool arm_mode, ready, psram;
  unsigned bits, max_samples;   // max_samples: the most samples a frame the memory takes (2: MASTER only)
  uint32_t mask;
  char take[40], prefix[25], error[64];
  uint32_t frames;         // this take's (or the last one's), silence included
  uint64_t bytes;          // written this take
  uint32_t peak[kMiniSources];   // 24-bit full scale = 8388607, since the last status that took them
  uint32_t drops, dropped_frames;   // ring overruns (blocks), their frames (written as silence)
  uint32_t write_kbps, worst_write_us, writes, busy_waits;   // the card inside the writes; polls it was busy
  uint32_t ring_bytes, block, ring_ms, ring_max_pct;   // a file's ring, its write, its length at the rate; high water
  uint64_t free_bytes, total_bytes;
  uint32_t need_kbps;      // the selection's data rate
};

class MiniRecorder {
 public:
  enum { kIdle = 0, kArmed = 1, kRecording = 2, kEnded = 3 };
  // mem: every file's ring (split between the files of a take); block: one write (a power of 2, >= 512);
  // max_samples: the most samples a frame allowed (2 without PSRAM: MASTER, or two mono channels)
  void init(void* mem, uint32_t bytes, uint32_t block, unsigned max_samples, bool psram, const MiniBackend* b);
  // control (the main loop)
  bool configure(uint32_t mask, unsigned bits, char* err, unsigned errlen);   // idle only
  bool set_prefix(const char* p);   // [A-Za-z0-9_-], 1..24
  bool start(char* err, unsigned errlen);    // now
  bool arm(char* err, unsigned errlen);      // on the next PLAY, stop on STOP; re-arms after each take
  void stop();                               // a take ends (its files closed by poll); armed: disarmed
  void transport(uint8_t status);            // MIDI realtime: FA start, FB continue, FC stop
  bool poll();                               // true: it used the card this call
  void status(MiniStatus* s, bool take_peaks);
  void refresh_space();                      // the card's free space asked again (idle only)
  uint32_t mask() const { return mask_; }
  unsigned bits() const { return bits_; }
  bool busy() const { return state_ != kIdle; }
  // producer (the Overbridge IN interrupt): n frames of `stride` int32 samples (full scale), IN 0..11 first
  void in_block(const int32_t* f, unsigned n, unsigned stride);
  // the boot: the take a power loss cut (kMarker) gets its true sizes; the files fixed
  int fix_pending();
  // the stream stopped this long while recording: the take ends (the Rytm unplugged or left Overbridge mode)
  static const uint32_t kStallUs = 1000000;
  static const uint32_t kGapStep = 64;   // silence frames put back a producer call, at most

 private:
  struct Out {
    int h;
    uint8_t* ring;
    volatile uint32_t wr, rd;   // bytes, free running; wr the producer's, rd the consumer's
    uint64_t data;              // written to the file
    uint8_t src, ch, width, fbytes;   // the source, its first IN channel, 1|2, bytes a frame
  };
  bool begin(bool arm, char* err, unsigned errlen, bool rearm = false);
  bool layout(uint32_t mask, char* err, unsigned errlen);
  bool open_step();
  bool finish_step();
  bool write_ring(Out& o, uint32_t n);
  void fail(const char* why);
  void end_take(const char* why);
  bool room(uint32_t frames) const;
  uint32_t ring_ms() const;
  void put(const int32_t* f, unsigned n, unsigned stride);
  bool cas(int from, int to) { return __atomic_compare_exchange_n(&state_, &from, to, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }

  const MiniBackend* b_ = nullptr;
  uint8_t* mem_ = nullptr;
  uint32_t mem_bytes_ = 0, block_max_ = 0, cap_ = 0, blk_ = 0;
  unsigned max_samples_ = 0;
  bool psram_ = false;
  volatile int state_ = kIdle;
  volatile uint8_t go_ = 0;
  bool arm_mode_ = false, open_ = false, stopping_ = false;
  uint32_t mask_ = 1u << kMaster;
  unsigned bits_ = 24;
  char prefix_[25] = "take", take_[40] = "", error_[64] = "";
  Out out_[kMiniSources];
  int nout_ = 0;
  // producer's
  volatile uint32_t gap_ = 0, frames_ = 0, in_frames_ = 0, drops_ = 0, dropped_frames_ = 0;
  volatile uint32_t peak_[kMiniSources] = {};
  // consumer's
  int phase_ = 0, next_ = 0, sync_next_ = 0;
  uint64_t bytes_ = 0, write_us_ = 0, free_ = 0, total_ = 0, free_at_start_ = 0;
  bool space_known_ = false;
  uint32_t worst_us_ = 0, writes_ = 0, busy_waits_ = 0, ring_max_ = 0;
  uint32_t last_sync_us_ = 0, last_in_us_ = 0, last_in_frames_ = 0, last_drops_ = 0, last_drop_log_us_ = 0;
};

}  // namespace rec
