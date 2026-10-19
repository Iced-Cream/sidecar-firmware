// RytmHost: see RytmHost.h.
#include "RytmHost.h"

#include <stdarg.h>
#include <stdio.h>

namespace {
inline void barrier() { __asm__ volatile("" ::: "memory"); }
}  // namespace

FLASHMEM void RytmOverbridge::init() {
  contribute_Pipes(mypipes, sizeof(mypipes) / sizeof(Pipe_t));
  contribute_Transfers(mytransfers, sizeof(mytransfers) / sizeof(Transfer_t));
  contribute_String_Buffers(mystrbuf, sizeof(mystrbuf) / sizeof(strbuf_t));
  memset(&st_, 0, sizeof st_);
  mk_setup(setup_[0], 0x01, 11, ob::kAlt, ob::kInIface, 0);   // SET_INTERFACE 1, alt 3
  mk_setup(setup_[1], 0x01, 11, ob::kAlt, ob::kOutIface, 0);  // SET_INTERFACE 2, alt 3
  mk_setup(setup_[2], 0x02, 1, 0, ob::kInEp, 0);              // CLEAR_FEATURE(ENDPOINT_HALT) 0x83
  mk_setup(setup_[3], 0x02, 1, 0, ob::kOutEp, 0);             // and 0x03
  driver_ready_for_device(this);
}

FLASHMEM const char* RytmOverbridge::stateName() const {
  switch (state_) {
    case kIdle: return "idle (no Overbridge device)";
    case kClaimed: return "claimed, waiting to start";
    case kSetIn: return "SET_INTERFACE 1 alt 3";
    case kSetOut: return "SET_INTERFACE 2 alt 3";
    case kClearIn: return "CLEAR_FEATURE halt 0x83";
    case kClearOut: return "CLEAR_FEATURE halt 0x03";
    case kStreaming: return "streaming";
    case kRefused: return "refused";
  }
  return "?";
}

FLASHMEM void RytmOverbridge::say(const char* fmt, ...) {
  if (!log_) return;
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  vsniprintf(b, sizeof b, fmt, ap);   // integers only: no float printf linked
  va_end(ap);
  log_(b);
}

// Offered by USBHost_t36 after SET_CONFIGURATION, in its interrupt: first the
// whole configuration (type 0), then each interface descriptor with all that
// follows it (type 1).
FLASHMEM bool RytmOverbridge::claim(Device_t* dev, int type, const uint8_t* d, uint32_t len) {
  if (dev->idVendor != ob::kVid) return false;
  if (type == 0) {   // keep a copy for bring-up; never claim the whole device
    cfg_len_ = len > sizeof cfg_ ? sizeof cfg_ : len;
    memcpy(cfg_, d, cfg_len_);
    vid_ = dev->idVendor;
    pid_ = dev->idProduct;
    speed_ = dev->speed;
    state_ = kIdle;
    refuse_why_ = nullptr;
    return false;
  }
  // Claim at interface 1's first (alt 0) descriptor; interface 2 is ours too,
  // and no other driver takes vendor interfaces without MIDI jacks.
  if (type != 1 || len < 9 || d[1] != 4 || d[2] != ob::kInIface || d[3] != 0) return false;
  ob::Layout l;
  if (!ob::find_layout(d, len, ob::kAlt, &l)) {
    refuse_why_ = "no interrupt endpoints 0x83/0x03 with whole blocks at alt 3 (not OVERBRIDGE mode?)";
    state_ = kRefused;
    return false;
  }
  if (dev->speed != 2) {
    refuse_why_ = "not high speed (Overbridge 2 needs 480 Mbit: cable, hub?)";
    state_ = kRefused;
    return false;
  }
  if (l.in_channels > ob::kChannels || l.out_channels > ob::kChannels) {
    refuse_why_ = "more than 12 channels (not a Rytm MKII?)";
    state_ = kRefused;
    return false;
  }
  layout_ = l;
  in_block_ = ob::block_bytes(l.in_channels);
  out_block_ = ob::block_bytes(l.out_channels);
  in_pipe_ = new_Pipe(dev, 3, l.in.addr & 0x0F, 1, l.in.mps, l.in.interval);
  out_pipe_ = in_pipe_ ? new_Pipe(dev, 3, l.out.addr & 0x0F, 0, l.out.mps, l.out.interval) : nullptr;
  if (!in_pipe_ || !out_pipe_) {
    refuse_why_ = "no pipe (interrupt bandwidth or pipe memory)";
    state_ = kRefused;
    in_pipe_ = out_pipe_ = nullptr;
    return false;
  }
  in_pipe_->callback_function = in_cb;
  out_pipe_->callback_function = out_cb;
  memset(&st_, 0, sizeof st_);
  st_.fresh = true;
  have_first_ = false;
  claim_ms_ = millis();
  start_wanted_ = auto_start_;
  state_ = kClaimed;
  return true;
}

// One setup_t per step: the controller reads the packet from it when it gets
// to it, which may be after a timed-out step's successor was queued.
FLASHMEM void RytmOverbridge::step() {
  if (state_ < kSetIn || state_ > kClearOut) return;
  step_ms_ = millis();
  queue_Control_Transfer(device, &setup_[state_ - kSetIn], nullptr, this);
}

FLASHMEM void RytmOverbridge::advance() {
  switch (state_) {
    case kSetIn: state_ = kSetOut; break;
    case kSetOut: state_ = kClearIn; break;
    case kClearIn: state_ = kClearOut; break;
    case kClearOut: start_stream(); return;
    default: return;
  }
  step();
}

// A setup request of ours completed (in the interrupt). Failed ones never come
// here (USBHost_t36 calls no driver on a control error): Task() times them out.
// Only the current step's own completion advances (a timed-out one finishing
// late does not); a CLEAR_FEATURE after a halt re-arms its pipe.
FLASHMEM void RytmOverbridge::control(const Transfer_t* t) {
  const uint32_t w1 = t->setup.word1, w2 = t->setup.word2;
  if (state_ >= kSetIn && state_ <= kClearOut) {
    const setup_t& s = setup_[state_ - kSetIn];
    if (w1 == s.word1 && w2 == s.word2) advance();
  } else if (state_ == kStreaming && clear_pending_ && w1 == clear_.word1 && w2 == clear_.word2) {
    clear_pending_ = false;
    if (clear_.wIndex == ob::kInEp) {
      if (rearm(in_pipe_, 1)) st_.in_rearms++;
    } else if (rearm(out_pipe_, 0)) {
      st_.out_rearms++;
    }
  }
}

FLASHMEM void RytmOverbridge::start_stream() {
  credit_ = 0;
  out_inflight_ = 0;
  out_head_ = 0;
  out_counter_ = 0;
  have_counter_ = false;
  primed_ = false;
  fifo_rd_ = fifo_wr_;
  stream_ms_ = last_in_ms_ = last_out_ms_ = millis();
  clear_pending_ = false;
  in_inflight_ = 0;
  state_ = kStreaming;
  for (int k = 0; k < kXfers; ++k)
    if (queue_Data_Transfer(in_pipe_, in_buf_[k], kBlocks * in_block_, this)) in_inflight_++;
}

void RytmOverbridge::in_cb(const Transfer_t* t) {
  if (t->driver) static_cast<RytmOverbridge*>(t->driver)->in_done(t);
}
void RytmOverbridge::out_cb(const Transfer_t* t) {
  if (t->driver) static_cast<RytmOverbridge*>(t->driver)->out_done(t);
}

void RytmOverbridge::in_done(const Transfer_t* t) {
  const uint32_t got = t->length - ((t->qtd.token >> 16) & 0x7FFF);
  const uint8_t* b = (const uint8_t*)t->buffer;
  const int ch = layout_.in_channels;
  const int nb = got / in_block_;
  if (in_inflight_ > 0) in_inflight_--;
  if (got % in_block_) st_.bad_blocks++;
  int good = 0, nf = 0;
  for (int i = 0; i < nb; ++i) {
    const uint8_t* blk = b + i * in_block_;
    if (!ob::block_from_device(blk)) {
      st_.bad_blocks++;
      continue;
    }
    uint16_t c = ob::block_counter(blk);
    if (have_counter_ && c != (uint16_t)(in_counter_ + ob::kFramesPerBlock)) st_.counter_gaps++;
    in_counter_ = c;
    have_counter_ = true;
    if (!have_first_) {
      memcpy(first_, blk, in_block_);
      have_first_ = true;
    }
    for (int f = 0; f < ob::kFramesPerBlock; ++f, ++nf)
      for (int k = 0; k < ch; ++k) {
        int32_t s = ob::block_sample(blk, ch, f, k);
        rx_frames_[nf * ch + k] = s;
        int32_t a = s < 0 ? (s == INT32_MIN ? INT32_MAX : -s) : s;
        if (a > st_.peak[k]) st_.peak[k] = a;
      }
    ++good;
  }
  st_.in_blocks += good;
  st_.in_frames += nf;
  last_in_ms_ = millis();
  if (rx_cb_ && nf) rx_cb_(rx_frames_, nf, ch);
  if (state_ != kStreaming || !device) return;
  if (queue_Data_Transfer(in_pipe_, t->buffer, t->length, this)) in_inflight_++;   // the same buffer again
  else st_.queue_fails++;   // Task() re-queues it
  if (out_enabled_) credit_ += good;
  pump_out();
}

void RytmOverbridge::out_done(const Transfer_t*) {
  if (out_inflight_ > 0) out_inflight_--;
  st_.out_blocks += kBlocks;
  st_.out_frames += kBlocks * ob::kFramesPerBlock;
  last_out_ms_ = millis();
  pump_out();
}

// As many OUT blocks as IN blocks came (Overwitch's and rytm-emu's pacing: the
// Rytm is the clock), kBlocks a transfer, at most kXfers in flight.
void RytmOverbridge::pump_out() {
  const int cap = 2 * kBlocks * kXfers;
  if (credit_ > cap) {
    st_.credit_drops += credit_ - cap;
    credit_ = cap;
  }
  while (out_enabled_ && credit_ >= kBlocks && out_inflight_ < kXfers && device && state_ == kStreaming) {
    uint8_t* buf = out_buf_[out_head_];
    const uint32_t rd = fifo_rd_;
    const uint16_t counter = out_counter_;
    fill_out(buf);
    if (!queue_Data_Transfer(out_pipe_, buf, kBlocks * out_block_, this)) {   // no Transfer_t free:
      fifo_rd_ = rd;                                                          // as if not taken
      out_counter_ = counter;
      st_.queue_fails++;
      break;
    }
    out_head_ = (out_head_ + 1) % kXfers;
    out_inflight_++;
    credit_ -= kBlocks;
  }
}

void RytmOverbridge::fill_out(uint8_t* buf) {
  const int ch = layout_.out_channels;
  const int mirror = mirror_;
  const uint8_t* map = map_;
  for (int i = 0; i < kBlocks; ++i) {
    uint8_t* blk = buf + i * out_block_;
    ob::write_header(blk, out_counter_);
    out_counter_ += ob::kFramesPerBlock;
    uint32_t rd = fifo_rd_;
    const uint32_t wr = fifo_wr_;
    barrier();
    const int level = (int)(wr - rd);
    if (level > 0) primed_ = true;
    if (level < st_.fifo_min) st_.fifo_min = level;
    if (level > st_.fifo_max) st_.fifo_max = level;
    int missing = 0;
    for (int f = 0; f < ob::kFramesPerBlock; ++f) {
      if (rd != wr) {
        ob::put_voice_frame(blk, ch, f, fifo_[rd & (kFifo - 1)], map, mirror);
        ++rd;
      } else {
        ob::put_voice_frame(blk, ch, f, nullptr, map, -1);
        ++missing;
      }
    }
    barrier();
    fifo_rd_ = rd;
    if (tx_cb_) add_tx(blk, ch);
    if (missing && primed_) {
      st_.xrun_blocks++;
      st_.xrun_frames += missing;
    }
  }
}

// fill_out's: the tx callback's frames added to one block (flash: about 7 x 12 samples, 0.3 us).
FLASHMEM void RytmOverbridge::add_tx(uint8_t* blk, int channels) {
  int32_t add[ob::kFramesPerBlock * ob::kChannels];
  if (channels > ob::kChannels) channels = ob::kChannels;
  memset(add, 0, sizeof add);
  tx_cb_(add, ob::kFramesPerBlock, channels);
  for (int f = 0; f < ob::kFramesPerBlock; ++f)
    for (int k = 0; k < channels; ++k) {
      int32_t a = add[f * channels + k];
      if (!a) continue;
      uint8_t* p = blk + ob::kHeaderBytes + 4 * (channels * f + k);
      int64_t s = (int64_t)ob::block_sample(blk, channels, f, k) + a;
      if (s > INT32_MAX) s = INT32_MAX;
      if (s < INT32_MIN) s = INT32_MIN;
      ob::put_be32(p, (int32_t)s);
    }
}

FLASHMEM void RytmOverbridge::setMainOnly(bool on) {
  __disable_irq();   // fill_out() (the interrupt) moves fifo_rd_ too
  if (on != mainOnly()) fifo_rd_ = fifo_wr_;
  map_ = on ? ob::kVoiceToNone : ob::kVoiceToRytm;
  mirror_ = on ? 0 : -1;
  __enable_irq();
}

int RytmOverbridge::pushVoices(const int16_t (*frames)[ob::kVoices], int n) {
  const uint32_t wr = fifo_wr_, rd = fifo_rd_;
  const int space = kFifo - (int)(wr - rd);
  if (n > space) n = space;
  for (int i = 0; i < n; ++i) memcpy(fifo_[(wr + i) & (kFifo - 1)], frames[i], sizeof fifo_[0]);
  barrier();
  fifo_wr_ = wr + n;
  return n;
}

FLASHMEM void RytmOverbridge::stats(Stats* s, bool reset) {
  __disable_irq();
  *s = st_;
  s->fifo_now = fifoLevel();
  if (reset) {
    st_.fifo_min = st_.fifo_max = s->fifo_now;
    st_.fresh = false;
    memset(st_.peak, 0, sizeof st_.peak);
  }
  __enable_irq();
}

FLASHMEM bool RytmOverbridge::firstBlock(uint8_t* out, int max) const {
  if (!have_first_) return false;
  memcpy(out, first_, max < in_block_ ? max : in_block_);
  return true;
}

// A halted interrupt qTD is never reported by USBHost_t36 and blocks its
// queue. USBHost_t36 leaves CERR 0 (bus errors retry forever, never halt), so a
// halt is a STALL, babble or a buffer error. Task() first sends the endpoint
// CLEAR_FEATURE(ENDPOINT_HALT) (a STALLed endpoint stays halted until then; it
// also resets the Rytm's toggle to DATA0), and its completion re-arms the qTD
// in place: the same buffer and length, active again, and the queue head's
// overlay cleared (DATA0) so the controller fetches it anew (EHCI 1.0,
// 4.10.2). The periodic followup list still holds it; it completes and is freed
// as usual. [untested: needs the Rytm and an error]
FLASHMEM bool RytmOverbridge::rearm(Pipe_t* pipe, uint32_t pid) {
  if (!pipe || !(pipe->qh.token & 0x40)) return false;
  Transfer_t* t = (Transfer_t*)(pipe->qh.current & ~0x1Fu);
  if (!t || !t->buffer || !t->length) return false;
  uint32_t a = (uint32_t)t->buffer;
  t->qtd.alt_next = 1;
  t->qtd.buffer[0] = a;
  a &= 0xFFFFF000;
  for (int k = 1; k < 5; ++k) t->qtd.buffer[k] = a + 0x1000 * k;
  t->qtd.token = (t->length << 16) | 0x8000 | (pid << 8) | 0x80;
  pipe->qh.next = (uint32_t)t;
  pipe->qh.alt_next = 1;
  pipe->qh.token = 0;
  return true;
}

FLASHMEM void RytmOverbridge::Task() {
  const uint32_t now = millis();
  if (state_ != said_state_) {
    said_state_ = state_;
    if (state_ == kClaimed)
      say(PSTR("ob: Rytm %04x:%04x, %d ch in / %d ch out, packets %u/%u B, bInterval %u/%u - %s"), vid_, pid_,
          layout_.in_channels, layout_.out_channels, layout_.in.mps, layout_.out.mps, layout_.in.interval,
          layout_.out.interval, start_wanted_ ? "starting" : "waiting for start()");
    else
      say(PSTR("ob: %s"), stateName());
  }
  if (state_ == kClaimed && start_wanted_ && now - claim_ms_ >= 20) {
    NVIC_DISABLE_IRQ(IRQ_USBHS);
    if (state_ == kClaimed) {
      state_ = kSetIn;
      step();
    }
    NVIC_ENABLE_IRQ(IRQ_USBHS);
    return;
  }
  if (state_ >= kSetIn && state_ <= kClearOut) {
    NVIC_DISABLE_IRQ(IRQ_USBHS);   // the condition again: a completion may have come
    const State s = state_;
    const bool late = s >= kSetIn && s <= kClearOut && millis() - step_ms_ > 300;
    if (late) {
      st_.setup_timeouts++;
      advance();
    }
    NVIC_ENABLE_IRQ(IRQ_USBHS);
    if (late) say(PSTR("ob: %s got no answer in 300 ms (stalled?), going on"), s == kSetIn ? "SET_INTERFACE 1" : s == kSetOut ? "SET_INTERFACE 2" : s == kClearIn ? "CLEAR_FEATURE 0x83" : "CLEAR_FEATURE 0x03");
    return;
  }
  if (state_ != kStreaming || now - said_ms_ < 1000) return;
  // the stream's health, at most once a second; pipes read with the interrupt
  // off (disconnect() clears them from it)
  uint32_t in_tok = 0, out_tok = 0, in_age = 0, out_age = 0;
  int requeued = 0;
  const char* cleared = nullptr;
  NVIC_DISABLE_IRQ(IRQ_USBHS);
  Pipe_t *ip = in_pipe_, *op = out_pipe_;
  if (state_ == kStreaming && ip && op && device) {
    in_tok = ip->qh.token;
    out_tok = op->qh.token;
    // the clock read again with the USB interrupt off: `now` is from Task()'s start, and a block completing since
    // stamps last_in_ms_ later than it (2026-10-19: "no IN block for 4294967295 ms", -1 wrapped, on a connect)
    const uint32_t t = millis();
    in_age = t - last_in_ms_;
    out_age = t - last_out_ms_;
    // IN transfers lost to a full Transfer_t pool: queue them again
    for (int k = in_inflight_; k < kXfers; ++k)
      if (queue_Data_Transfer(ip, in_buf_[k], kBlocks * in_block_, this)) {
        in_inflight_++;
        requeued++;
      }
    if (clear_pending_ && now - clear_ms_ > 300) clear_pending_ = false;   // no answer: again
    bool in_halt = in_age > 50 && (in_tok & 0x40), out_halt = out_inflight_ > 0 && out_age > 50 && (out_tok & 0x40);
    if (!clear_pending_ && (in_halt || out_halt)) {
      mk_setup(clear_, 0x02, 1, 0, in_halt ? ob::kInEp : ob::kOutEp, 0);   // CLEAR_FEATURE(ENDPOINT_HALT)
      clear_pending_ = true;
      clear_ms_ = now;
      queue_Control_Transfer(device, &clear_, nullptr, this);
      cleared = in_halt ? "IN 0x83" : "OUT 0x03";
    }
  }
  NVIC_ENABLE_IRQ(IRQ_USBHS);
  if (in_age > 50 || cleared || requeued) {
    said_ms_ = now;
    if (in_age > 50) say(PSTR("ob: no IN block for %lu ms (IN qh token %08lx, OUT %08lx)"), (unsigned long)in_age,
                         (unsigned long)in_tok, (unsigned long)out_tok);
    if (cleared) say(PSTR("ob: %s halted: CLEAR_FEATURE sent, re-armed when it completes"), cleared);
    if (requeued) say(PSTR("ob: %d IN transfers queued again (Transfer_t pool was full)"), requeued);
  }
}

FLASHMEM void RytmOverbridge::disconnect() {
  state_ = kIdle;
  in_pipe_ = out_pipe_ = nullptr;
  credit_ = 0;
  out_inflight_ = 0;
  fifo_rd_ = fifo_wr_;
}
