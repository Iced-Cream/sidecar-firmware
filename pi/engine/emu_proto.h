// pi/engine/emu_proto.h - rytm-emu's Overbridge link (rytm-emu src/oblink.rs), the host's side, without sockets: a
// parser for what the emulator sends and the headers of what the host sends. tcp_link.cpp puts it on a socket.
#pragma once
#include <stdint.h>

class EmuParser {
 public:
  struct Sink {
    virtual ~Sink() {}
    virtual void Want(uint32_t frames) = 0;            // 'N': the emulator wants this many more frames
    virtual void Midi(const uint8_t* d, int n) = 0;    // 'M': the Rytm's USB MIDI output (any split)
    virtual void Hello(uint8_t granted) = 0;           // 'H': the answer to the host's hello
  };
  explicit EmuParser(Sink* s) : sink_(s) {}
  // false: a byte that starts no known message. Sticky: once false, every later Feed is false and the stream is
  // lost - the caller must close the socket (and Reset() the parser before reusing it on a reconnect).
  bool Feed(const uint8_t* d, int n);
  void Reset() { kind_ = 0; need_ = have_ = 0; body_ = 0; bad_ = false; }   // a fresh stream (skipped_ is kept)
  bool Bad() const { return bad_; }
  uint64_t Skipped() const { return skipped_; }        // 'T', 'P', 'R' bytes ignored (the stand-ins, the Rytm's audio)

 private:
  Sink* sink_;
  uint8_t kind_ = 0;          // 0: between messages
  uint8_t head_[4];
  int need_ = 0, have_ = 0;   // header bytes
  uint64_t body_ = 0;         // payload bytes left ('M' passed on, 'R' skipped; 64-bit: 48 * a u32 count)
  bool bad_ = false;
  uint64_t skipped_ = 0;
};

int emu_hello(uint8_t* out, uint8_t flags);                       // 'H' 1 flags
int emu_midi_head(uint8_t* out, uint16_t len);                    // 'M' len, then len bytes
int emu_frames_head(uint8_t* out, uint8_t kind, uint32_t n);      // 'A' (12 x i32) or 'a' (8 x i16) + n, then the frames
