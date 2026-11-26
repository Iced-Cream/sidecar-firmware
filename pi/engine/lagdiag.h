// pi/engine/lagdiag.h - lag diagnostics for the black box (notes from the Rytm-lag hunt; docs/pi-engine.md "Lag diagnostics").
// Portable, header only, no allocation (tests/test_lagdiag.cpp). Three parts, each one small class and a formatter:
//   1 the "Lag now" marker (Marker, Snap): the tester presses a button when the Rytm feels slow; one black box line says when,
//     and one more holds the per-second counters as they stood
//   2 the Rytm's report stream (RxDiag): how the Rytm's own 03 / 04 reports arrive
//   3 what the Pi sends the Rytm (TxDiag)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if __has_include(<circle/memorymap.h>)   // the kernel: no snprintf, platform_pi.cpp's subset (%u %02u %s)
extern "C" int core_snprintf(char* out, size_t n, const char* fmt, ...);
#define LAGDIAG_FMT core_snprintf
#else
#include <stdio.h>
#define LAGDIAG_FMT snprintf
#endif

namespace lagdiag {

// ---- 1. the marker -------------------------------------------------------------------------------------------------

// The console command for it: exactly this, no argument. A client can not put text into the log with it.
inline bool is_marker_cmd(const char* c) { return c != nullptr && strcmp(c, "lagnow") == 0; }

// One marker a second at most (a button held down or double-tapped writes one line); N counts the accepted ones per boot.
class Marker {
 public:
  static constexpr uint32_t kMinMs = 1000;
  // now_ms: any 32-bit millisecond clock (it may wrap). The new N, or 0 when this press came too soon after the last.
  unsigned press(uint32_t now_ms) {
    if (n_ != 0 && (uint32_t)(now_ms - last_ms_) < kMinMs) {
      ++refused_;
      return 0;
    }
    last_ms_ = now_ms;
    return ++n_;
  }
  unsigned count() const { return n_; }
  unsigned refused() const { return refused_; }

 private:
  unsigned n_ = 0, refused_ = 0;
  uint32_t last_ms_ = 0;
};

inline unsigned cut(char* out, unsigned max, int w) {   // snprintf's result as the length really written
  if (max == 0) return 0;
  if (w < 0) w = 0, out[0] = 0;
  return (unsigned)w < max ? (unsigned)w : max - 1;
}

// "lag-marker N at HH:MM:SS.cc": the same uptime clock as the log's own time stamps
inline unsigned fmt_marker(char* out, unsigned max, unsigned n, uint32_t uptime_ms) {
  if (max == 0) return 0;
  uint32_t cs = uptime_ms / 10, s = cs / 100;
  return cut(out, max, LAGDIAG_FMT(out, max, "lag-marker %u at %02u:%02u:%02u.%02u", n, (unsigned)(s / 3600), (unsigned)(s / 60 % 60),
                                (unsigned)(s % 60), (unsigned)(cs % 100)));
}

constexpr unsigned kLine = 200;   // a diagnostic line's text at most (the log's own line limit is far above it)

// The engine: line's figures as they stood at the last second (kernel.cpp LogStats fills it), and the last lines of parts 2 and 3
struct Snap {
  unsigned fifo_min, under, block_max_us;
  unsigned c1, c2, c3;   // the cores' busy share, tenths of a percent
  unsigned pass_max_us, midi_bytes, sysex;
  unsigned sent_bytes, sent_msgs, sent_fail, dropped;
  char rx[kLine], tx[kLine];   // the last rxrep / txacc line text ("" none)
};

inline unsigned fmt_snap(char* out, unsigned max, const Snap& s) {
  if (max == 0) return 0;
  int w = LAGDIAG_FMT(out, max,
                   "lag-snap: fifo min %u under %u | block max %u us | cpu c1 %u.%u c2 %u.%u c3 %u.%u %% | pass max %u us | "
                   "midi %u B sysex %u | to-host %u B %u msgs fail %u dropped %u",
                   s.fifo_min, s.under, s.block_max_us, s.c1 / 10, s.c1 % 10, s.c2 / 10, s.c2 % 10, s.c3 / 10, s.c3 % 10, s.pass_max_us,
                   s.midi_bytes, s.sysex, s.sent_bytes, s.sent_msgs, s.sent_fail, s.dropped);
  unsigned n = cut(out, max, w);
  if (s.rx[0] && n + 1 < max) n += cut(out + n, max - n, LAGDIAG_FMT(out + n, max - n, " | rx: %s", s.rx));
  if (s.tx[0] && n + 1 < max) n += cut(out + n, max - n, LAGDIAG_FMT(out + n, max - n, " | tx: %s", s.tx));
  return n;
}

// ---- 2. the Rytm's report stream ---------------------------------------------------------------------------------------

// The Rytm sends its voices' parameters itself (rytm-mods 0021: 03 PARAMS, 04 TRIG+PARAMS, and the MIDI machine's 43 / 44), up to
// 94 a second a voice. The load on the Rytm's side is invisible to the Pi, but a Rytm whose audio interrupt runs late sends them
// late: a growing gap between two reports of one voice (the Rytm's cap is a clock of its own, 16 blocks, ~10.7 ms) is that, seen
// from here. RxDiag aggregates, per second:
//   per voice that reported: the count, the longest gap between two reports of that voice (us), and a mask of the parameter
//     words (P1..P16 = bits 0..15) that differed from the voice's previous report at least once
//   every SysEx from the Rytm (reports or not): the count, the longest gap between two (us), and the jitter: the mean change
//     between one gap and the next (us)
// WHICH CORE. on_sysex and tick run on the control core (core 3), where the Rytm's MIDI bytes are parsed (cores.cpp FromRytm,
// and the pass loop for tick): fixed arrays, no allocation, no logging, a few dozen instructions a report. take runs on core 0's
// once-a-second logger, which prints the line. The two meet in one published second, a seqlock (a reader that meets a write
// takes nothing and tries again a second later: it never blocks the writer, never reads a half-written second).
// The time is the control core's when it handles the message: a message that sat in the 8 KB MIDI queue while a long pass ran
// arrives late by that, which is itself what is wanted (the pass max is in the engine: line).
class RxDiag {
 public:
  static constexpr unsigned kVoices = 8;
  static constexpr uint64_t kSecondUs = 1000000;
  static constexpr uint64_t kStreamGapUs = 1000000;   // a longer gap is a pause, not a late report: it only restarts the measuring

  struct Second {   // one second of the stream; all words, for the seqlock
    uint32_t n[kVoices], gap_us[kVoices], mask[kVoices];
    uint32_t sx_n, sx_max_us, sx_jit_us;
  };

  // A SysEx from the Rytm (F0 .. F7), now_us a microsecond clock
  void on_sysex(uint64_t now_us, const uint8_t* d, int len) {
    roll_if_due(now_us);
    if (!open_) open_ = true, start_ = now_us;
    ++cur_.sx_n;
    if (sx_seen_) {
      uint64_t iv = now_us - sx_last_;
      if (iv <= kStreamGapUs) {
        if (iv > cur_.sx_max_us) cur_.sx_max_us = (uint32_t)iv;
        if (iv_seen_) jit_sum_ += iv > prev_iv_ ? iv - prev_iv_ : prev_iv_ - iv, ++jit_n_;
        prev_iv_ = iv, iv_seen_ = true;
      } else {
        iv_seen_ = false;
      }
    }
    sx_last_ = now_us, sx_seen_ = true;
    // a report: F0 7D 52 54 <cmd> <machine | voice> ..., the layout of core/voices.cpp voice_report
    if (len < 23 || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54) return;
    const uint8_t cmd = d[4];
    const uint8_t* raw;
    unsigned np;
    if (cmd == 0x04 || cmd == 0x44) {
      if (len < 25) return;
      raw = d + 8, np = len >= 41 ? 16 : 8;
    } else if (cmd == 0x03 || cmd == 0x43) {
      raw = d + 6, np = len >= 39 ? 16 : 8;
    } else {
      return;
    }
    const unsigned vi = d[5] & 7;
    Voice& v = voice_[vi];
    ++cur_.n[vi];
    if (v.seen) {
      uint64_t gap = now_us - v.last;
      if (gap <= kStreamGapUs && gap > cur_.gap_us[vi]) cur_.gap_us[vi] = (uint32_t)gap;
    }
    v.last = now_us, v.seen = true;
    uint32_t changed = 0;
    for (unsigned i = 0; i < np; ++i) {
      if (i < v.known && (raw[2 * i] != v.w[2 * i] || raw[2 * i + 1] != v.w[2 * i + 1])) changed |= 1u << i;
      v.w[2 * i] = raw[2 * i], v.w[2 * i + 1] = raw[2 * i + 1];
    }
    if (np > v.known) v.known = np;
    cur_.mask[vi] |= changed;
  }

  // The control core's pass: a second without a message ends too
  void tick(uint64_t now_us) { roll_if_due(now_us); }

  // The Rytm went away (core 3, with bridge::rytm_gone): its parameter words and report times are not compared with the next
  // ones after it comes back
  void forget() {
    for (Voice& v : voice_) v.known = 0, v.seen = false;
    sx_seen_ = iv_seen_ = false;
  }

  // Core 0: the newest second not taken yet, true if there is one. Only seconds with a report in them are ever published.
  bool take(Second* out) {
    uint32_t s1 = __atomic_load_n(&seq_, __ATOMIC_ACQUIRE);
    if ((s1 & 1) || s1 == taken_) return false;
    uint32_t* o = reinterpret_cast<uint32_t*>(out);
    for (unsigned i = 0; i < kWords; ++i) o[i] = __atomic_load_n(&pub_[i], __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&seq_, __ATOMIC_RELAXED) != s1) return false;   // a write met the read: next time
    taken_ = s1;
    return true;
  }

 private:
  static constexpr unsigned kWords = sizeof(Second) / sizeof(uint32_t);
  static_assert(sizeof(Second) % sizeof(uint32_t) == 0, "Second is words");
  struct Voice {
    uint64_t last;
    bool seen;
    unsigned known;   // words of w that have a value
    uint8_t w[32];
  };

  void roll_if_due(uint64_t now_us) {
    // (a time not after start_ is a pass that stamped its start before the message that opened the second: not a second)
    if (open_ && now_us > start_ && now_us - start_ >= kSecondUs) roll();
  }
  void roll() {
    bool active = false;
    for (unsigned v = 0; v < kVoices; ++v) active |= cur_.n[v] != 0;
    if (active) {
      cur_.sx_jit_us = jit_n_ ? (uint32_t)(jit_sum_ / jit_n_) : 0;
      const uint32_t* c = reinterpret_cast<const uint32_t*>(&cur_);
      uint32_t s = seq_;
      __atomic_store_n(&seq_, s + 1, __ATOMIC_RELAXED);
      __atomic_thread_fence(__ATOMIC_RELEASE);
      for (unsigned i = 0; i < kWords; ++i) __atomic_store_n(&pub_[i], c[i], __ATOMIC_RELAXED);
      __atomic_store_n(&seq_, s + 2, __ATOMIC_RELEASE);
    }
    memset(&cur_, 0, sizeof cur_);
    jit_sum_ = jit_n_ = 0;
    open_ = false;
  }

  Second cur_ = {};
  Voice voice_[kVoices] = {};
  uint64_t start_ = 0, sx_last_ = 0, prev_iv_ = 0, jit_sum_ = 0, jit_n_ = 0;
  bool open_ = false, sx_seen_ = false, iv_seen_ = false;
  uint32_t seq_ = 0, taken_ = 0;
  uint32_t pub_[kWords] = {};
};

// "rxrep: v5 n94 gap10700us m0003 | v6 n93 gap10900us m0ff0 | sx n121 max11200us jit410us": the voices that reported only, the gap
// and the changed-words mask (hex, P1 = bit 0) of each (the longest gap first), then every SysEx's figures. Capped to max bytes (and kLine by the caller's
// choice of max): voices that do not fit are left out and counted as "+N more".
inline unsigned fmt_rx(char* out, unsigned max, const RxDiag::Second& x) {
  if (max == 0) return 0;
  char sx[64];
  unsigned sxn = cut(sx, sizeof sx, LAGDIAG_FMT(sx, sizeof sx, " | sx n%u max%uus jit%uus", x.sx_n, x.sx_max_us, x.sx_jit_us));
  unsigned n = cut(out, max, LAGDIAG_FMT(out, max, "rxrep:"));
  unsigned left = 0;
  // the voices that reported, the longest gap first (a stable order: equal gaps by voice), so a cut never hides the late one
  unsigned order[RxDiag::kVoices], k = 0;
  for (unsigned v = 0; v < RxDiag::kVoices; ++v) {
    if (x.n[v] == 0) continue;
    unsigned j = k++;
    for (; j > 0 && x.gap_us[order[j - 1]] < x.gap_us[v]; --j) order[j] = order[j - 1];
    order[j] = v;
  }
  for (unsigned i = 0; i < k; ++i) {
    const unsigned v = order[i];
    char item[56];
    unsigned in = cut(item, sizeof item, LAGDIAG_FMT(item, sizeof item, " %sv%u n%u gap%uus m%04x", n > 6 ? "| " : "", v, x.n[v], x.gap_us[v], x.mask[v] & 0xFFFF));
    if (left || n + in + 12 + sxn >= max) {   // (12: " | +N more")
      ++left;
      continue;
    }
    memcpy(out + n, item, in + 1);
    n += in;
  }
  if (left && n + 12 + sxn < max) n += cut(out + n, max - n, LAGDIAG_FMT(out + n, max - n, " | +%u more", left));
  if (n + sxn < max) memcpy(out + n, sx, sxn + 1), n += sxn;
  return n;
}

// ---- 3. what the Pi sends the Rytm ---------------------------------------------------------------------------------------

// The engine's records for the Rytm (g_ToHost, sent by usbmidi_merge.h on core 0), by what they are. The Pi's remote UI talks to
// the Rytm in SysEx F0 7D 52 54 <cmd> ... (core/ui/remote.cpp): 3C the keepalive, 38 / 3D / 3E / 36 the frames (rects, text, text
// clear, splash), 39 the commit, 48 the parameter writes; other commands and the host-link replies are "sx", anything else
// (the LFOs' CCs, a piece of a long SysEx) "oth".
enum Kind : unsigned { kKeepalive, kFrame, kCommit, kWrite, kOtherSx, kOther, kKinds };

inline Kind classify(const uint8_t* rec, int len) {
  if (rec == nullptr || len <= 0 || rec[0] != 0xF0) return kOther;
  if (len < 6 || rec[1] != 0x7D || rec[2] != 0x52 || rec[3] != 0x54) return kOtherSx;
  switch (rec[4]) {
    case 0x3C: return kKeepalive;
    case 0x38: case 0x3D: case 0x3E: case 0x36: return kFrame;
    case 0x39: return kCommit;
    case 0x48: return kWrite;
    default: return kOtherSx;
  }
}

// Per second, on core 0 (the merge's Poll and the logger are both its): the records sent by type (count, bytes of the message, not
// of the USB packets), the peak of the engine queue's bytes waiting at the start of a Poll ("pend"), and the longest a bulk send
// to the Rytm took (the Circle transfer blocks core 0 until the Rytm's USB MIDI endpoint accepts it: "wait"; the Rytm's mods hold
// their output too, and this is how long that hold looks from here). No allocation, no logging; one array increment a record.
class TxDiag {
 public:
  struct Second {
    uint32_t n[kKinds], bytes[kKinds], pend_max, wait_max_us;
  };
  static constexpr uint32_t kWaitLineUs = 2000;   // a wait this long is a line even when only keepalives went

  void record(const uint8_t* rec, int len) {
    Kind k = classify(rec, len);
    ++cur_.n[k];
    cur_.bytes[k] += len > 0 ? (uint32_t)len : 0;
  }
  void pending(unsigned bytes) {
    if (bytes > cur_.pend_max) cur_.pend_max = bytes;
  }
  void waited(uint32_t us) {
    if (us > cur_.wait_max_us) cur_.wait_max_us = us;
  }
  // Since the last take. False when nothing worth a line happened: no record but keepalives (every ~0.7 s, so a line for each
  // would be noise) and no wait of kWaitLineUs or more.
  bool take(Second* out) {
    bool worth = cur_.wait_max_us >= kWaitLineUs;
    for (unsigned k = 0; k < kKinds; ++k) worth |= k != kKeepalive && cur_.n[k] != 0;
    *out = cur_;
    cur_ = Second{};
    return worth;
  }

 private:
  Second cur_ = {};
};

// "txacc: ka n1 b7 | frm n3 b78 | c39 n1 b8 | w48 n2 b20 | sx n1 b15 | oth n1 b3 | pend 4096 B | wait 840us": the types that sent
// something, then the peak queue and the longest send. Capped to max bytes (a type that does not fit is left out).
inline unsigned fmt_tx(char* out, unsigned max, const TxDiag::Second& x) {
  static const char* const kName[kKinds] = {"ka", "frm", "c39", "w48", "sx", "oth"};
  if (max == 0) return 0;
  char tail[64];
  unsigned tn = cut(tail, sizeof tail, LAGDIAG_FMT(tail, sizeof tail, "pend %u B | wait %uus", x.pend_max, x.wait_max_us));
  unsigned n = cut(out, max, LAGDIAG_FMT(out, max, "txacc: "));
  for (unsigned k = 0; k < kKinds; ++k) {
    if (x.n[k] == 0) continue;
    char item[48];
    unsigned in = cut(item, sizeof item, LAGDIAG_FMT(item, sizeof item, "%s n%u b%u | ", kName[k], x.n[k], x.bytes[k]));
    if (n + in + tn >= max) continue;
    memcpy(out + n, item, in + 1);
    n += in;
  }
  if (n + tn < max) memcpy(out + n, tail, tn + 1), n += tn;
  return n;
}

}   // namespace lagdiag
