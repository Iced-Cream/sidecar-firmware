// core/ui/page_slots.cpp - see extra.h: the USER SLOTS page, which user machine (a /Plugins package) each slot U1..U5
// plays, on the box's own screen. For a box with the remote UI and the user slots (device.h kUserSlots: the Rytm, the
// A4), on the LEFT / RIGHT ring after SCOPE, SAMPLE and AUTO (the A4's last: LEFT on SIDECAR) and in the SIDECAR MENU; shown only where the platform has the user machines
// (remote_slots: core/user/um.cpp installs them, the Pi and the Mac). Cold.
//
// Rows U1..U5, each the machine its slot plays as the box shows it ("---" empty, "!NAME" missing, "NAME..." loading).
//   UP / DOWN or knob A   the row
//   knob B                a machine for the row from the installed list ("---" first: the slot cleared), one entry a
//                         detent (list_turn); the row shows it with a "?" until YES or NO
//   YES                   the row's slot plays it (or is cleared): the console's um=assign:<slot>:<id> / um=clear:<slot>,
//                         the web page's own commands, so the slot map, the fingerprints, the save and the project's
//                         list behave as they do there
//   NO                    a pick dropped; with none, back to SIDECAR (as every page)
//   knobs C..H            taken and ignored (not the stock parameters under the page)
// The pick is dropped when the row changes and when the page has not been drawn for kStaleMs (it was left). It holds
// the machine's id and name, not only its list index: a rescan between knob B and YES (a package uploaded from the web
// meanwhile) can reorder the list, and YES assigns the machine shown (rc040 review L4).
#include <string.h>

#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

enum : int { kYes = 0x44, kNo = 0x45, kUp = 0x42, kDown = 0x43 };
constexpr int kSlots = 5, kRowY = 11, kRowH = 8;
constexpr uint32_t kNoteMs = 2000, kStaleMs = 500;

const RemoteSlots* hooks = nullptr;

struct State {
  int row;            // the slot (0..4)
  int pick;           // the machine picked for it: -1 none, 0 "---" (cleared), 1.. installed entry pick - 1
  bool note_new;      // a note set by a key: timed from the next frame
  uint32_t note_ms, drawn_ms;
  char note[26];
  char pick_id[32];   // pick > 0: the picked machine's package id (um.cpp Pkg::id) and name, as knob B found them
  char pick_name[16];
};
State S = {0, -1, false, 0, 0, {}, {}, {}};

CORE_COLD int machines() { return hooks && hooks->machines ? hooks->machines() : 0; }
CORE_COLD const char* machine_name(int i, const char** id) {   // installed entry i, null past the end
  static char b[16];
  *id = "";
  return hooks && hooks->machine && hooks->machine(i, b, sizeof b, id) ? b : nullptr;
}
// The list entry of what slot s plays now: 0 empty (or not in the installed list: a missing package), 1.. the entry
CORE_COLD int current(int s) {
  char nm[16];
  const char* id = hooks && hooks->slot ? hooks->slot(s, nm, sizeof nm) : "";
  if (!id || !id[0]) return 0;
  for (int i = 0, n = machines(); i < n; ++i) {
    const char* mid;
    if (machine_name(i, &mid) && !strcmp(mid, id)) return i + 1;
  }
  return 0;
}
// The list entry (1..) whose id is the pick's now, 0 when it is gone from the list
CORE_COLD int pick_entry() {
  for (int i = 0, n = machines(); i < n; ++i) {
    const char* mid;
    if (machine_name(i, &mid) && !strcmp(mid, S.pick_id)) return i + 1;
  }
  return 0;
}
CORE_COLD void set_pick(int p) {   // the pick is list entry p (0: "---"): its id and name held
  const char* id = "";
  const char* nm = p > 0 ? machine_name(p - 1, &id) : nullptr;
  S.pick = p;
  sniprintf(S.pick_id, sizeof S.pick_id, "%s", nm ? id : "");
  sniprintf(S.pick_name, sizeof S.pick_name, "%s", nm ? nm : "");
}
CORE_COLD void set_note(const char* t) {
  sniprintf(S.note, sizeof S.note, "%s", t);
  S.note_new = true;
}

CORE_COLD void slots_draw(Canvas& c, int page, int npages, uint32_t ms) {
  if (S.drawn_ms && ms - S.drawn_ms > kStaleMs) S.pick = -1;   // the page was left: no pick waits
  S.drawn_ms = ms | 1;
  c.clear();
  header(c, "USER SLOTS", page, npages, nullptr);
  if (S.row < 0) S.row = 0;
  if (S.row >= kSlots) S.row = kSlots - 1;
  const int n = machines();
  if (S.pick > 0) {   // the list may have been rescanned: the pick follows its machine (gone: still shown, YES says so)
    const int e = pick_entry();
    if (e) S.pick = e;
    else if (S.pick > n) S.pick = n > 0 ? n : 1;
  }
  for (int s = 0; s < kSlots; ++s) {
    const bool on = s == S.row;
    const int y = kRowY + s * kRowH;
    if (on) c.fill(Rect{0, (int16_t)(y - 1), 128, (int16_t)(kRowH - 1)}, kOn);
    const uint8_t inv = on ? kTextInvert : 0;
    char l[4], v[20], nm[16];
    sniprintf(l, sizeof l, "U%d", s + 1);
    nm[0] = 0;
    if (hooks && hooks->slot) hooks->slot(s, nm, sizeof nm);
    if (on && S.pick >= 0) {
      sniprintf(v, sizeof v, "%s?", S.pick ? S.pick_name : "---");
    } else {
      sniprintf(v, sizeof v, "%s", nm[0] ? nm : "---");
    }
    c.put_text(kSlotFree + 2 * s, kRSmall, 2, y, l, (uint8_t)(kAlignLeft | inv));
    c.put_text(kSlotFree + 2 * s + 1, kRSmall, 126, y, v, (uint8_t)(kAlignRight | inv));
  }
  char f[26];
  if (S.note_new || (S.note_ms && ms - S.note_ms < kNoteMs)) sniprintf(f, sizeof f, "%s", S.note);
  else if (S.pick >= 0) sniprintf(f, sizeof f, "YES: %s U%d  NO: KEEP", S.pick ? "SET" : "CLEAR", S.row + 1);
  else if (!n) sniprintf(f, sizeof f, "NO MACHINES IN /PLUGINS");
  else sniprintf(f, sizeof f, "B: MACHINE  YES: SET");
  footer(c, f);
  if (S.note_new) S.note_new = false, S.note_ms = ms | 1;
}

CORE_COLD void slots_enc(int e, int delta, bool) {
  const int l = e == 0 || e == 1 ? list_turn(e, delta) : 0;
  if (!l) return;
  if (e == 0) {
    int r = S.row + l;
    r = r < 0 ? 0 : r >= kSlots ? kSlots - 1 : r;
    if (r != S.row) S.row = r, S.pick = -1;
    return;
  }
  const int n = machines();
  const int from = S.pick < 0 ? current(S.row) : S.pick > 0 && pick_entry() ? pick_entry() : S.pick;
  int p = from + l;   // the first detent moves from what the slot plays
  set_pick(p < 0 ? 0 : p > n ? n : p);
  S.note_new = false, S.note_ms = 0;   // the bottom bar says what YES does now
}

// The slot takes the pick: the console's (and the web page's) command, its answer as the note
CORE_COLD void take() {
  if (!hooks || !hooks->command) return;
  char c[64], out[120];
  if (S.pick > 0 && S.pick_id[0]) sniprintf(c, sizeof c, "um=assign:%d:%s", S.row + 1, S.pick_id);   // the id held
  else if (S.pick == 0) sniprintf(c, sizeof c, "um=clear:%d", S.row + 1);
  else return;
  out[0] = 0;
  const bool ok = hooks->command(c, out, sizeof out);
  if (!ok) set_note("NOT HERE");
  else if (out[0] == 'U') {   // "U2 = <id>", "U2 cleared"
    char b[26];
    if (S.pick && S.pick_name[0]) sniprintf(b, sizeof b, "U%d = %s", S.row + 1, S.pick_name);
    else sniprintf(b, sizeof b, "U%d CLEARED", S.row + 1);
    set_note(b);
  } else if (!strncmp(out, "after", 5)) set_note("SET WHEN THE MAP IS READ");   // a project change's map being read
  else if (strstr(out, "loading")) set_note("BUSY: AGAIN IN A MOMENT");
  else set_note("NOT INSTALLED");
  S.pick = -1;
}

CORE_COLD bool slots_key(int code) {
  if (code == kUp || code == kDown) {
    int r = S.row + (code == kUp ? -1 : 1);
    r = r < 0 ? 0 : r >= kSlots ? kSlots - 1 : r;
    if (r != S.row) S.row = r, S.pick = -1;
    return true;
  }
  if (code == kYes) {
    if (S.pick >= 0) take();
    else set_note(machines() ? "B: PICK A MACHINE" : "NO MACHINES IN /PLUGINS");
    return true;
  }
  if (code == kNo && S.pick >= 0) {
    S.pick = -1;
    set_note("KEPT");
    return true;
  }
  return false;   // (NO: back to SIDECAR; the arrows navigate)
}

CORE_COLD bool slots_shown() { return hooks != nullptr; }

}  // namespace

// encoders A..H all taken (C..H do nothing here): a stray turn does not edit the stock parameter under the page
UI_ROM const ExtraPage kSlotsPage = {"USER SLOTS", slots_shown, slots_draw, slots_enc, slots_key, 0xFF, false, nullptr,
                                     nullptr, nullptr, false};

CORE_COLD void remote_slots(const RemoteSlots* h) { hooks = h; }
CORE_COLD const RemoteSlots* remote_slots() { return hooks; }
CORE_COLD void slots_page_reset() { S = State{0, -1, false, 0, 0, {}, {}, {}}; }
CORE_COLD int slots_page_row() { return S.row; }
CORE_COLD int slots_page_pick() { return S.pick; }

}  // namespace ui
}  // namespace core
