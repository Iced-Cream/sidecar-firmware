// core/pidisplay/pidisplay.h - the Pi-attached display as a third screen target (S8k, port plan 9k; scope section 8 "Pi-attached
// display", decided 2026-11-28): the same drawing service as the boxes' screens (core/screen's Client: a 128x64 ui::Canvas,
// keys back), drawn scaled up on a panel the Pi drives, with ONE Pi-wide stack that clients of both boxes push onto.
// No LVGL and no pages of its own: an idle frame when the stack is empty, a soft-key bar for touches.
//
// The stack follows the box stacks' rules (Projects\Sidecar\notes\design-s8h-screen-stack-status-2026-11-28.md section 2):
//   - every request goes on top (the latest request is shown and takes the keys); a client already on the stack is moved to
//     the top (no duplicates); at most kDepth entries;
//   - the entry below keeps its state and gets no draw or key calls while covered;
//   - close removes the entry wherever it is; closing the top shows the one below again with a whole frame; an empty stack
//     shows the idle frame;
//   - callbacks as S8h's Pi side gives them (core/screen on g2b-s8h-screen-stack): opened (true) when an entry is new,
//     opened (false) when it leaves; nothing when covered or shown again. S8h adds Client::shown (bool): once it is on
//     generic-two-box, a covered top gets shown (false) and a top shown again shown (true) here too (one line each in
//     push / close);
//   - any client may open itself (Q3: allowed; documentation recommends a box mod opens Pi pages from the box);
//   - Pi-wide popups (the pairing code) draw over the top entry and do not enter the stack; the top redraws after them.
// An entry is (slot, client): slot 0/1 a box's client, kPiWide a client of the Pi's own. The same client may be open on a
// box's screen too; its draw is called for each target, its keys come from whichever target the user touches.
//
// Keys: the soft keys LEFT UP DOWN RIGHT NO YES go to the top client as its box's key codes (key_code: the Rytm's codes
// until S8h's per-box key tables, set_keymap); CLOSE closes the top entry (the display has no stock exit of its own). There
// are no encoders on the display. A touch while a popup is up ends the popup and goes no further.
//
// Threads: everything on the control side (the Pi's core 3, as core/screen); the Pi side hands touches over from its USB
// IRQ core (pi/engine/pi_display.cpp). The panel's put is called from tick(). Portable: tested on the host with a fake
// panel (core/tests/test_pidisplay.cpp).
#pragma once
#include <stdint.h>

#include "screen/screen.h"
#include "ui/ui.h"

namespace core {
namespace pidisplay {

// ---- the panel (the Pi side's frame buffer, or a test's)
struct Panel {
  int w, h;   // pixels
  // RGB565 pixels at (x, y), w x h, rows `stride` pixels apart; always inside the panel
  void (*put)(void* ctx, int x, int y, int w, int h, const uint16_t* px, int stride);
  void* ctx;
};
// The panel to draw on; null: none (the stack is still kept, nothing is drawn). False: the panel is too small for the
// canvas at scale 1 with a soft-key bar (128 x 64 + kMinBar). Drawn whole at the next tick.
bool attach(const Panel* p);
bool attached();

// ---- layout: the canvas scaled by a whole number and centred across the top, the soft-key bar under it
constexpr int kMaxScale = 10, kMinBar = 40;
struct Layout {
  int scale;            // canvas pixel -> scale x scale panel pixels
  int cx, cy;           // the canvas's top left on the panel
  int bar_y, bar_h;     // the soft-key bar: full width, from bar_y to the bottom
  int key_w;            // each soft key's width (the last one takes the remainder)
};
Layout layout();        // all 0 without a panel

// ---- the stack
constexpr int kDepth = 8;
constexpr int kPiWide = -1;
bool push(int slot, const screen::Client* c);    // on top (moved there if already on it); false: null client or full
bool close(int slot, const screen::Client* c);   // wherever it is; false: not on the stack
bool close_top();
void clear();                                    // every entry closed (opened (false) to each, top first)
int depth();
const screen::Client* top(int* slot = nullptr);  // null: empty
bool on_stack(int slot, const screen::Client* c);
void redraw();                                   // the top's frame changed: drawn at the next tick (also polled, kPollMs)

// ---- Pi-wide popups (the pairing code): over the top entry, every touch ends them. hold_ms 0 = 0.8 s, at most 30 s.
// False: no panel attached (nothing is kept for later).
bool popup(const char* l1, const char* l2 = nullptr, uint32_t hold_ms = 0);
void popup_end();
bool popup_up();

// ---- keys
enum Key : uint8_t { kClose, kLeft, kUp, kDown, kRight, kNo, kYes, kKeys };   // the soft keys, left to right
using KeyMap = int (*)(int slot, Key k);   // the code the client's key() gets, < 0: none
void set_keymap(KeyMap m);                 // null: rytm_keys (S8h's per-box key tables replace it)
int rytm_keys(int slot, Key k);            // LEFT 0x40 RIGHT 0x41 UP 0x42 DOWN 0x43 YES 0x44 NO 0x45
int key_at(int x, int y);                  // the soft key under panel pixel (x, y), -1: none

// ---- input: one finger, panel pixels (after the touch calibration, touch_cal.h)
void touch(int x, int y, bool down);

// The address for the idle frame ("" none)
void set_address(const char* ip);

constexpr uint32_t kTickMs = 25, kPollMs = 100;
void tick(uint32_t ms);

struct Stats { uint32_t pushes, closes, frames, tiles, keys, touches, popups, full; };
Stats stats();
void reset();   // tests: no panel, empty stack, no popup, the Rytm's key map

// The colours (RGB565)
constexpr uint16_t kInk = 0xFFFF, kPaper = 0x0000, kKeyBg = 0x2945, kKeyDown = 0x8410, kKeyEdge = 0x5ACB, kKeyOff = 0x630C;
}  // namespace pidisplay
}  // namespace core
