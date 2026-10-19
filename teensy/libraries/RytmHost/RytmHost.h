// RytmHost: the Teensy 4.1 as the USB host of an Analog Rytm MKII in
// OVERBRIDGE mode, on PJRC's USBHost_t36 (Teensyduino). Our own code (MIT
// intended); the protocol knowledge is in ob_block.h.
//
// What it does:
//   - claims interface 1 of an Elektron (VID 0x1935) high-speed device whose
//     interfaces 1 and 2 have, at alternate setting 3, interrupt endpoints
//     0x83 / 0x03 carrying whole Overbridge blocks (368 bytes: 12 channels);
//   - SET_INTERFACE 1 and 2 to alt 3, CLEAR_FEATURE(ENDPOINT_HALT) on 0x83 and
//     0x03 (as Overwitch does), then keeps kXfers IN transfers queued;
//   - for every block the Rytm sends, sends one back (the Rytm is the clock: it
//     sends 48000 / 7 blocks a second and gets as many) carrying the 8 voices
//     from a FIFO the application fills (pushVoices), on each voice's own input
//     channel (ob::kVoiceToRytm); an empty FIFO sends silence and counts an xrun.
//   - The USB MIDI interface (5) is left to USBHost_t36's MIDIDevice_BigBuffer
//     (it claims class 1/3 interfaces itself); the Rytm's descriptors pass its
//     checks (ob_block.h's notes, docs/HOST-BRINGUP.md).
//
// What USBHost_t36 lacks and this works around (in this file, not the library):
//   - no audio class and no Overbridge: Overbridge 2 needs no isochronous, only
//     high-speed interrupt pipes (new_Pipe type 3, bInterval 1 = every
//     microframe, one <= 1024-byte packet per microframe - the library's
//     high-bandwidth mult is fixed at 1, enough for 368 bytes);
//   - errors on periodic (interrupt) pipes are never reported: followup_Error()
//     only walks the async list, so a halted interrupt qTD stays queued forever
//     and the stream just stops. Task() watches for that, sends the endpoint
//     CLEAR_FEATURE(ENDPOINT_HALT) and re-arms the halted qTD when it completes
//     (rearm(); [untested without the Rytm]).
//   - control transfers that fail (STALL) never call back: Task() times each
//     setup step out and goes on.
// Callbacks run in the USB host interrupt; the FIFO is single-producer
// (loop()) single-consumer (the interrupt). Buffers live in RAM1 (DTCM, not
// cached), so no cache maintenance is needed - do not make an instance DMAMEM.
#pragma once
#include <Arduino.h>
#include <USBHost_t36.h>

#include "ob_block.h"

#ifndef RYTM_OB_BLOCKS_PER_XFER
#define RYTM_OB_BLOCKS_PER_XFER 2   // blocks per IN/OUT transfer (7 frames each)
#endif
#ifndef RYTM_OB_XFERS
#define RYTM_OB_XFERS 4             // transfers in flight per direction
#endif
#ifndef RYTM_OB_FIFO
#define RYTM_OB_FIFO 512            // host -> Rytm voice FIFO, frames (power of 2)
#endif

class RytmOverbridge : public USBDriver {
public:
  enum State : uint8_t { kIdle, kClaimed, kSetIn, kSetOut, kClearIn, kClearOut, kStreaming, kRefused };
  static constexpr int kBlocks = RYTM_OB_BLOCKS_PER_XFER;
  static constexpr int kXfers = RYTM_OB_XFERS;
  static constexpr int kFifo = RYTM_OB_FIFO;
  static_assert((kFifo & (kFifo - 1)) == 0, "RYTM_OB_FIFO must be a power of 2");
  static_assert(kBlocks * ob::block_bytes(ob::kChannels) <= 16384, "one qTD a transfer (rearm())");

  struct Stats {
    uint32_t in_blocks, out_blocks;     // blocks moved (since attach)
    uint32_t in_frames, out_frames;
    uint32_t bad_blocks;                // wrong magic or a short packet
    uint32_t counter_gaps;              // the Rytm's frame counter skipped
    uint32_t xrun_blocks, xrun_frames;  // OUT blocks the FIFO could not fill
    uint32_t credit_drops;              // IN blocks not answered (OUT not taken)
    uint32_t in_rearms, out_rearms;     // halted pipes re-armed
    uint32_t setup_timeouts;            // setup steps that got no answer
    uint32_t queue_fails;               // transfers not queued (Transfer_t pool empty)
    bool fresh;                         // counters restarted (a new attach) since the last stats()
    int fifo_now, fifo_min, fifo_max;   // since the last stats(reset = true)
    int32_t peak[ob::kChannels];        // Rytm -> host, |sample| max, since reset
  };

  RytmOverbridge(USBHost&) { init(); }

  // A line of text (no newline). Task() calls it, never the interrupt.
  void setLog(void (*log)(const char* line)) { log_ = log; }
  // false: claim and wait for start() (MIDI-only bring-up in OVERBRIDGE mode).
  void setAutoStart(bool on) { auto_start_ = on; }
  void start() { start_wanted_ = true; }
  // false: answer no IN block (the OUT stream off), for bring-up.
  void setOutEnabled(bool on) { out_enabled_ = on; }
  // Voice `v` also to MAIN L/R (bring-up: audible without a trig), -1 off.
  void setMainMirror(int v) { mirror_ = v; }
  // true: each frame's slot 0 goes to MAIN L/R and the voice channels stay silent (a stock Rytm plays only MAIN;
  // the application pushes its MAIN mix in slot 0, core/main_out.h); false: each voice on its own channel. A
  // change drops the frames queued (made for the other layout: never a raw voice on MAIN). It sets the mirror
  // (setMainMirror) to 0 / off.
  void setMainOnly(bool on);
  bool mainOnly() const { return map_ == ob::kVoiceToNone; }

  State state() const { return state_; }
  const char* stateName() const;
  bool streaming() const { return state_ == kStreaming; }
  int channelsIn() const { return layout_.in_channels; }
  int channelsOut() const { return layout_.out_channels; }
  uint8_t speed() const { return speed_; }   // 0 12 Mbit, 1 1.5, 2 480
  const char* refused() const { return refuse_why_; }   // why kRefused

  // The FIFO, host -> Rytm: frames of 8 voices (physical voice order), int16.
  int fifoLevel() const { return (int)(fifo_wr_ - fifo_rd_); }
  int pushVoices(const int16_t (*frames)[ob::kVoices], int n);

  void stats(Stats* s, bool reset);
  // Rytm -> host, from the interrupt: n frames of `channels` int32 samples
  // (native endian), after each IN transfer.
  void setRxCallback(void (*cb)(const int32_t* frames, int n, int channels)) { rx_cb_ = cb; }
  // host -> Rytm, from the interrupt, once a block (7 frames): fill frames[n][channels] (int32, native endian)
  // with what to add to the block's samples (saturating) - another source's channels, e.g. a computer's over
  // UAC2 (the host build's USB_SERIAL_UAC2). Added after the voices, on every channel. nullptr: none.
  void setTxCallback(void (*cb)(int32_t* frames, int n, int channels)) { tx_cb_ = cb; }

  // Bring-up: the Elektron device's configuration descriptor as enumerated
  // (without its 9-byte header), and the first IN block seen.
  const uint8_t* config(uint16_t* len) const { *len = cfg_len_; return cfg_; }
  uint16_t vid() const { return vid_; }
  uint16_t pid() const { return pid_; }
  const ob::Layout& layout() const { return layout_; }
  bool firstBlock(uint8_t* out, int max) const;

protected:
  bool claim(Device_t* dev, int type, const uint8_t* descriptors, uint32_t len) override;
  void control(const Transfer_t* transfer) override;
  void disconnect() override;
  void Task() override;

private:
  void init();
  void step();                         // issue the setup request for state_
  void advance();                      // the next setup step, or the stream
  void start_stream();
  static void in_cb(const Transfer_t* t);
  static void out_cb(const Transfer_t* t);
  void in_done(const Transfer_t* t);
  void out_done(const Transfer_t* t);
  void pump_out();
  void fill_out(uint8_t* buf);
  bool rearm(Pipe_t* pipe, uint32_t pid);
  void say(const char* fmt, ...);

  Pipe_t mypipes[3] __attribute__((aligned(32)));
  // in flight: 2 x kXfers, a halt qTD per pipe, 3 per control transfer (two
  // at most: a late setup step and the next)
  Transfer_t mytransfers[2 * kXfers + 10] __attribute__((aligned(32)));
  strbuf_t mystrbuf[1];

  Pipe_t* in_pipe_ = nullptr;
  Pipe_t* out_pipe_ = nullptr;
  setup_t setup_[4];                   // the steps kSetIn..kClearOut (init())
  setup_t clear_;                      // CLEAR_FEATURE after a halt while streaming
  volatile bool clear_pending_ = false;
  uint32_t clear_ms_ = 0, said_ms_ = 0;
  volatile int in_inflight_ = 0;
  ob::Layout layout_;
  int in_block_ = 0, out_block_ = 0;   // bytes a block
  volatile State state_ = kIdle;
  uint32_t step_ms_ = 0, claim_ms_ = 0;
  bool auto_start_ = true;
  volatile bool start_wanted_ = false;
  volatile bool out_enabled_ = true;
  volatile int mirror_ = -1;
  const uint8_t* volatile map_ = ob::kVoiceToRytm;   // the voices' channels: kVoiceToNone in main-only mode
  uint8_t speed_ = 0;
  uint16_t vid_ = 0, pid_ = 0;
  void (*log_)(const char* line) = nullptr;
  void (*rx_cb_)(const int32_t*, int, int) = nullptr;
  void (*volatile tx_cb_)(int32_t*, int, int) = nullptr;
  void add_tx(uint8_t* blk, int channels);
  State said_state_ = kIdle;
  const char* refuse_why_ = nullptr;

  // IN: kXfers buffers, re-queued from their own callback.
  uint8_t in_buf_[kXfers][kBlocks * ob::block_bytes(ob::kChannels)] __attribute__((aligned(32)));
  // OUT: a ring of kXfers buffers, completions come back in order.
  uint8_t out_buf_[kXfers][kBlocks * ob::block_bytes(ob::kChannels)] __attribute__((aligned(32)));
  int out_head_ = 0;
  volatile int out_inflight_ = 0;
  volatile int credit_ = 0;            // IN blocks not answered yet
  uint16_t out_counter_ = 0;
  bool have_counter_ = false;
  uint16_t in_counter_ = 0;
  volatile uint32_t last_in_ms_ = 0, last_out_ms_ = 0, stream_ms_ = 0;

  int16_t fifo_[kFifo][ob::kVoices];
  volatile uint32_t fifo_wr_ = 0, fifo_rd_ = 0;
  bool primed_ = false;                // the FIFO has held frames once

  Stats st_;
  uint8_t first_[ob::block_bytes(ob::kChannels)];
  volatile bool have_first_ = false;
  int32_t rx_frames_[kBlocks * ob::kFramesPerBlock * ob::kChannels];

  uint8_t cfg_[512];
  uint16_t cfg_len_ = 0;
};
