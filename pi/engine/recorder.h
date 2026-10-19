// pi/engine/recorder.h - multitrack recording to the card (docs/pi-engine.md "Recording"): the selected channels of
// the Overbridge IN stream (MAIN L/R, the 8 voice stems, AUDIO IN L/R), the master pair, the Sidecar's 8 voices and
// the two send FX returns, one WAV a source in /Recordings/<take>/<NN>_<NAME>.wav, every file starting on the same
// frame and the same length (sample-aligned: they drop into a DAW lined up).
//
// Three sides, no locks, no allocation after Init:
//   - producer (the IN completion, core 0's xHCI interrupt; the self-test's loop): in_block() copies the selected
//     samples of each IN block (with the voices/FX frames out_block() queued from the OUT fill) into a ring of
//     fixed slots in memory reserved once at boot. A full ring drops the block, counted; the consumer writes the
//     dropped frames as silence, so the files keep time (and stay aligned). Never blocks, never logs.
//   - control (core 0: the console, the web page; transport() from any core): configure, arm, start, stop.
//   - consumer (core 0's loop, poll()): one 192 KB write a call at most (a sector-aligned, whole-chunk write: the
//     headers are 512 bytes), so the loop (network, display) keeps running between writes.
// Portable: the card is a Backend (FatFs on the Pi: rec_pi.cpp; stdio in tests/test_recorder.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "rec/rec_names.h"   // the sources, channel lists, take names and arming, shared with the Sidecar Mini's
#include "spsc.h"

namespace rec {

const unsigned kBlockFrames = 7;   // one Overbridge block
const uint32_t kChunk = 196608;     // a file's write: 192 KB (whole frames for 2, 3, 4 and 6 bytes)

struct Backend {
  bool (*mkdir)(const char* path);                      // true if made or there
  int (*open)(const char* path);                        // create (truncate) for writing: a handle 0..kSources-1, -1
  bool (*write)(int h, const void* p, uint32_t n);      // at the end
  bool (*pwrite)(int h, uint64_t off, const void* p, uint32_t n);   // the header, then back to the end
  bool (*sync)(int h);
  bool (*close)(int h);
  bool (*space)(uint64_t* free, uint64_t* total);
  uint32_t (*now_us)();
  // the boot scan and the take numbering
  int (*list)(const char* dir, char (*names)[64], uint64_t* sizes, bool* dirs, int max);
  bool (*read_at)(const char* path, uint64_t off, void* p, uint32_t n);
  bool (*patch)(const char* path, uint64_t off, const void* p, uint32_t n);
  void (*log)(const char* line);
};

struct Status {
  int state;            // 0 idle, 1 armed (waiting for PLAY or start), 2 recording, 3 finishing (draining, closing)
  bool arm_mode, ready; // ready: memory reserved
  unsigned bits;
  uint32_t mask;
  char take[40], prefix[25], error[64];
  uint64_t frames;      // recorded (this take, or the last one)
  uint32_t peak[kSources];   // 24-bit full scale = 8388607, since the last status (reset by it)
  uint32_t drops, dropped_frames, side_under;   // ring overruns (blocks), OUT side frames missing
  uint64_t bytes;       // written this take
  uint32_t write_kbps;  // the card's speed inside the writes (KB/s), this take
  uint32_t worst_write_us, writes;
  uint32_t ring_slots, ring_max_used, ring_used;   // slots; the high-water mark this take
  uint32_t ring_ms;     // the ring's length at the selection's rate
  uint64_t free_bytes, total_bytes;
  uint32_t need_kbps;   // the selection's data rate
};

class Recorder {
 public:
  // mem: the ring and the staging (kSources * kChunk), reserved once; bytes == 0: recording off (refused)
  void init(void* mem, size_t bytes, const Backend* b);
  // control (core 0)
  bool configure(uint32_t mask, unsigned bits, char* err, unsigned errlen);   // idle only
  bool set_prefix(const char* p);   // [A-Za-z0-9_-], 1..24
  bool start(char* err, unsigned errlen);    // now (manual)
  bool arm(char* err, unsigned errlen);      // start on the next PLAY (transport 0xFA/0xFB), stop on STOP; re-arms
  void stop();                               // a take in progress ends (its files closed by poll); armed: disarmed
  void transport(uint8_t status);            // MIDI realtime, any core: FA start, FB continue, FC stop
  void poll();                               // core 0's loop
  void status(Status* s, bool take_peaks);
  uint32_t mask() const { return mask_; }
  unsigned bits() const { return bits_; }
  // producer (the IN completion): n <= kBlockFrames frames of the 12 IN channels (int32 full scale)
  void in_block(const int32_t (*in)[kInChannels], unsigned n);
  // the OUT fill (before or after in_block, the same core): the voices as rendered (int16), the FX returns (float)
  void out_block(const int16_t (*voices)[kVoices], const float (*fx)[4], unsigned n);
  bool wants_side() const { return side_on_ && state_ != 0; }
  bool wants_in() const { return state_ == 1 || state_ == 2; }
  // the boot scan: every WAV of ours under dir/<take>/ with the sizes its length says (a take cut by a power loss);
  // returns the files fixed
  int fix_all(const char* dir);

  enum { kIdle = 0, kArmed = 1, kRecording = 2, kEnded = 3 };

 private:
  struct Out { int h; uint8_t* buf; uint32_t fill; uint64_t data; uint8_t width, src; };
  bool begin(bool arm, char* err, unsigned errlen, bool rearm = false);
  bool open_take();
  void close_take();
  bool write_out(Out& o);
  void fail(const char* why);
  bool cas(int from, int to) { return __atomic_compare_exchange_n(&state_, &from, to, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
  int32_t* slot(uint32_t i) { return ring_ + (size_t)(i & (nslots_ - 1)) * slot_words_; }

  const Backend* b_ = nullptr;
  uint8_t* stage_ = nullptr;
  int32_t* ring_ = nullptr;
  size_t ring_bytes_ = 0;
  uint32_t nslots_ = 0, slot_words_ = 0, nsamp_ = 0;
  volatile int state_ = kIdle;
  volatile uint8_t go_ = 0;
  bool arm_mode_ = false, side_on_ = false, open_ = false, stopping_ = false;
  uint32_t mask_ = 1u << kMaster;
  unsigned bits_ = 24;
  char prefix_[25] = "take", take_[40] = "", error_[64] = "";
  // producer's
  uint32_t wr_ = 0, gap_ = 0;   // wr_: __atomic (the consumer reads it)
  volatile uint32_t drops_ = 0, dropped_frames_ = 0, side_under_ = 0;
  volatile uint32_t peak_[kSources] = {};
  struct Side { int32_t s[kSide]; };
  Spsc<Side, 512> side_;
  // consumer's
  volatile uint32_t rd_ = 0;
  uint32_t ring_max_ = 0, frame_in_slot_ = 0, zeros_ = 0;
  Out out_[kSources];
  int nout_ = 0, sync_next_ = 0, flush_next_ = 0;
  uint64_t frames_ = 0, bytes_ = 0, write_us_ = 0;
  uint32_t worst_us_ = 0, writes_ = 0, last_sync_us_ = 0, last_space_us_ = 0;
  uint64_t free_ = 0, total_ = 0;
  uint32_t last_drops_ = 0, last_drop_log_us_ = 0;
};

}  // namespace rec
