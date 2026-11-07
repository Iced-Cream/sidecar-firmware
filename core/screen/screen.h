// core/screen/screen.h - the remote screen as a generic transport (2026-11-04, the engine strip-down): the Rytm's 128x64
// panel driven over 30..3F (rytm-mods 0026's screen part, unchanged on the wire; sim/protocol.py, core/ui/README.md),
// with no page of its own. What the engine itself shows is the connect splash at the Rytm's hello and short popups
// (the pairing code); everything else is a client's: one Client draws a frame into a ui::Canvas and takes the keys and
// encoders while the page is open. The engine's own client is the status page (status_page.h, 2026-11-07). With no client, a page the Rytm opens is closed again at once (3A 0 0: stock).
// core/ui/remote.cpp's pages stay in the tree, unwired.
//
// Wire (the 0026 subset this uses):
//   Rytm -> host  30 <page> <on> <reason>   31 <key hi> <key lo> <down>   32 <enc> <delta + 64> <func>
//                 35 <uptime hi> <uptime lo> <caps>   (3B <kit> and the rest: ignored)
//   host -> Rytm  38 tiles, 3D / 3E text, 39 commit, 3A <page> <on>, 3C keepalive, 37 the catch mask, 36 <ticks> splash
// Threads: sysex() and tick() on the control side (the Pi's core 3). Portable.
#pragma once
#include <stdint.h>

#include "ui/ui.h"

namespace core {
namespace screen {

struct Client {
  const char* name;
  void (*draw)(ui::Canvas& c, uint32_t ms);       // the whole frame (the transport diffs it against what was sent)
  void (*key)(int code, bool down);               // 31 (only while the page is open)
  void (*enc)(int e, int delta, bool func);       // 32
  void (*opened)(bool on, int page);              // 30: the Rytm opened (page) / closed it
  // the catch mask (37, kMaskBytes): which keys and encoders the page takes; null: the navigation keys and every encoder
  const uint8_t* mask;
};
constexpr int kMaskBytes = 3 + 37;
void set_client(const Client* c);   // null: none (a page the Rytm opens goes back to stock)
const Client* client();
void request(bool open);            // 3A 0 <open>: the host opens / closes the page
void redraw();                      // the client's frame changed: drawn at the next tick (at most every 25 ms)
bool open();

// The Rytm's hello (35): caps (bit 0: it understands 36), how many so far
int caps();
uint32_t hellos();
bool greeted();

// The connect splash's lines (the network's address: "" none, then "USB AUDIO+MIDI")
void set_address(const char* ip);

// A popup on the Rytm's screen (through 36, every key and knob staying the Rytm's): hold_ms 0 = 0.8 s, at most 30 s;
// over: drawn over an open page too (the pairing code), else refused while a page is open, the splash runs or an
// `over` popup is owed. False: the Rytm cannot show one (no 36 in its hello) or it was refused.
constexpr uint32_t kPopupMaxMs = 30000;
bool popup(const char* l1, const char* l2 = nullptr, uint32_t hold_ms = 0, bool over = false);
void popup_end();
uint32_t popups();          // shown so far
uint32_t status_popups();   // of them, not `over`

bool sysex(const uint8_t* d, int len);   // a whole 30..3F: true if it was one
void tick(uint32_t ms);
void resync();                           // records lost: everything owed goes again (a whole frame, the mask)
void reset();                            // tests

struct Stats { uint32_t opens, closes, frames, tiles, keepalives, keys, encs, hellos, splashes, bad, bytes; };
Stats stats();
}  // namespace screen
}  // namespace core
