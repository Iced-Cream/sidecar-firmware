// core/tests/test_noteroute.cpp - note routes (core/noteroute, port plan S8l): route by device and channel, only into
// the asking box's own slots, the USB-C port only for its own box, held notes released on move / unroute / unplug /
// link reset, a freed slot, one delivery per slot, refusals (full, bad, not the owner), 2B DEVICES, and thru through
// core/devmidi unchanged with routes open (devmidi's listener feeding the service, as the wiring will).
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "devmidi/devmidi.h"
#include "noteroute/noteroute.h"

using namespace core;
namespace nr = core::noteroute;

struct Ev { int box, slot; uint8_t s, a, b; };
static std::vector<Ev> g_ev;
static void sink(int box, int slot, uint8_t s, uint8_t a, uint8_t b) { g_ev.push_back({box, slot, s, a, b}); }
static const char* g_dev[4] = {"", "", "", ""};
static const char* names(int src) { return src >= 0 && src < 4 ? g_dev[src] : ""; }

static std::vector<uint8_t> msg(std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x51, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static bool send(int box, std::vector<uint8_t> body) {
  auto m = msg(body);
  return nr::sysex(box, m.data(), (int)m.size());
}
static std::vector<uint8_t> take(int box) {
  uint8_t a[160];
  int n = nr::take_answer(box, a, sizeof a);
  return std::vector<uint8_t>(a, a + n);
}
// NOTES / RENOTE's answer: the route (>= 0) or -code
static int answer(int box, uint8_t seq, uint8_t owner) {
  auto a = take(box);
  CHECK(a.size() == 11 && a[4] == 0x51 && a[5] == 1 && a[7] == seq && a[8] == owner && a[10] == 0xF7);
  CHECK(take(box).empty());
  if (a[6] == nr::kNoted) return a[9];
  CHECK(a[6] == nr::kRefused);
  return -a[9];
}
static int notes(int box, uint8_t seq, uint8_t owner, uint8_t src, uint8_t chan, uint8_t slot) {
  send(box, {nr::kNotes, seq, owner, src, chan, slot});
  return answer(box, seq, owner);
}
static bool has(int box, int slot, uint8_t s, uint8_t a, uint8_t b) {
  for (const Ev& e : g_ev) if (e.box == box && e.slot == slot && e.s == s && e.a == a && e.b == b) return true;
  return false;
}
static void fresh() {
  nr::clear_all();
  g_ev.clear();
}

int main() {
  nr::set_sink(sink);
  nr::set_names(names);

  // the op block: 28-2B box -> Pi, clear of S8h's 18-1C and the route's 20-24
  CHECK(nr::is_op(0x28) && nr::is_op(0x2B) && !nr::is_op(0x27) && !nr::is_op(0x2C) && !nr::is_op(0x18) && !nr::is_op(0x20));

  // ---- route by device and channel ----
  fresh();
  CHECK(notes(0, 1, 9, 2, 3, 5) == 0);     // USB-A device 2, channel 4 (3), box 0's slot 5
  nr::feed(2, -1, 0x93, 60, 100);
  CHECK(g_ev.size() == 1 && has(0, 5, 0x93, 60, 100));
  nr::feed(1, -1, 0x93, 61, 100);          // another device: no
  nr::feed(2, -1, 0x94, 62, 100);          // another channel: no
  nr::feed(2, -1, 0xF8, 0, 0);             // realtime: never
  CHECK(g_ev.size() == 1);
  nr::feed(2, -1, 0xE3, 0, 80);            // bend, CC, pressure travel with the notes
  nr::feed(2, -1, 0xB3, 1, 30);
  nr::feed(2, -1, 0xD3, 40, 0);
  CHECK(g_ev.size() == 4 && has(0, 5, 0xE3, 0, 80) && has(0, 5, 0xB3, 1, 30) && has(0, 5, 0xD3, 40, 0));
  nr::feed(2, -1, 0x83, 60, 0);
  CHECK(has(0, 5, 0x83, 60, 0) && nr::route(0, 0).held == 0);
  nr::feed(2, -1, 0x83, 99, 0);            // a note-off for a note never delivered goes nowhere
  CHECK(g_ev.size() == 5);
  CHECK(nr::stats(0).open == 1 && nr::stats(0).opened == 1 && nr::stats(1).open == 0);

  // any device, any channel
  fresh();
  CHECK(notes(1, 2, 4, nr::kUsbAAny, nr::kAnyChan, 7) == 0);
  nr::feed(0, -1, 0x90, 60, 1);
  nr::feed(3, -1, 0x9F, 61, 1);
  CHECK(g_ev.size() == 2 && has(1, 7, 0x90, 60, 1) && has(1, 7, 0x9F, 61, 1));
  // note-on velocity 0 is a note-off
  nr::feed(0, -1, 0x90, 60, 0);
  CHECK(has(1, 7, 0x90, 60, 0) && nr::route(1, 0).held == 1);

  // ---- only into the asking box: box 1's route never reaches box 0, box 0 cannot touch box 1's routes ----
  fresh();
  CHECK(notes(1, 3, 4, 0, nr::kAnyChan, 2) == 0);
  nr::feed(0, -1, 0x90, 60, 100);
  CHECK(g_ev.size() == 1 && g_ev[0].box == 1 && g_ev[0].slot == 2);
  send(0, {nr::kRenote, 4, 4, 0, 0, nr::kAnyChan, 9});   // box 0 names route 0: its own table has none open
  CHECK(answer(0, 4, 4) == -nr::kBad);
  CHECK(!send(0, {nr::kUnnote, 4, nr::kEvery}));
  CHECK(nr::route(1, 0).open && nr::route(1, 0).slot == 2);
  // both boxes routing the same keyboard: each gets it in its own slot (thru-like, by their own choice)
  CHECK(notes(0, 5, 1, 0, nr::kAnyChan, 2) == 0);
  g_ev.clear();
  nr::feed(0, -1, 0x91, 64, 90);
  CHECK(g_ev.size() == 2 && has(0, 2, 0x91, 64, 90) && has(1, 2, 0x91, 64, 90));

  // ---- the USB-C port: only the asking box's own ----
  fresh();
  CHECK(notes(0, 6, 1, nr::kUsbC, 0, 3) == 0);
  nr::feed(nr::kUsbC, 1, 0x90, 60, 100);   // the computer on box 1's port: not box 0's route
  CHECK(g_ev.empty());
  nr::feed(nr::kUsbC, 0, 0x90, 60, 100);
  CHECK(g_ev.size() == 1 && has(0, 3, 0x90, 60, 100));
  nr::feed(0, -1, 0x90, 61, 100);          // a USB-A device does not match a USB-C route
  CHECK(g_ev.size() == 1);
  CHECK(notes(0, 7, 1, 0x11, 0, 3) == -nr::kBad);   // no way to name another box's port
  nr::gone(nr::kUsbC, 1);                  // box 1's port going leaves box 0's notes alone
  CHECK(g_ev.size() == 1 && nr::route(0, 0).held == 1);
  nr::gone(nr::kUsbC, 0);
  CHECK(g_ev.size() == 2 && has(0, 3, 0x80, 60, 0) && nr::route(0, 0).held == 0);

  // ---- held notes: move ----
  fresh();
  CHECK(notes(0, 8, 2, 1, 0, 4) == 0);
  nr::feed(1, -1, 0x90, 60, 100);
  nr::feed(1, -1, 0x90, 64, 100);
  nr::feed(1, -1, 0xB0, 64, 127);          // sustain down
  g_ev.clear();
  send(0, {nr::kRenote, 9, 2, 0, 1, 0, 6});   // SELECTED moved: slot 4 -> slot 6
  CHECK(answer(0, 9, 2) == 0);
  CHECK(g_ev.size() == 3 && has(0, 4, 0x80, 60, 0) && has(0, 4, 0x80, 64, 0) && has(0, 4, 0xB0, 64, 0));
  CHECK(nr::route(0, 0).slot == 6 && nr::route(0, 0).held == 0 && nr::stats(0).moved == 1);
  g_ev.clear();
  nr::feed(1, -1, 0x80, 60, 0);            // the key let go after the move: nothing to slot 6 (it never had it)
  CHECK(g_ev.empty());
  nr::feed(1, -1, 0x90, 67, 100);
  CHECK(g_ev.size() == 1 && has(0, 6, 0x90, 67, 100));
  // move by someone else / of a closed route: refused
  send(0, {nr::kRenote, 10, 3, 0, 1, 0, 7});
  CHECK(answer(0, 10, 3) == -nr::kBad);
  send(0, {nr::kRenote, 11, 2, 5, 1, 0, 7});
  CHECK(answer(0, 11, 2) == -nr::kBad);

  // ---- held notes: unroute ----
  g_ev.clear();
  CHECK(send(0, {nr::kUnnote, 2, 0}));
  CHECK(g_ev.size() == 1 && has(0, 6, 0x80, 67, 0) && !nr::route(0, 0).open && nr::stats(0).open == 0);
  g_ev.clear();
  nr::feed(1, -1, 0x80, 67, 0);
  CHECK(g_ev.empty());

  // UNNOTE 7F closes every route of that owner, no other owner's
  fresh();
  CHECK(notes(0, 1, 5, 0, 0, 1) == 0);
  CHECK(notes(0, 2, 5, 1, 0, 2) == 1);
  CHECK(notes(0, 3, 6, 2, 0, 3) == 2);
  CHECK(send(0, {nr::kUnnote, 5, nr::kEvery}));
  CHECK(!nr::route(0, 0).open && !nr::route(0, 1).open && nr::route(0, 2).open && nr::stats(0).open == 1);

  // ---- held notes: unplug, through core/devmidi (its listener feeding the service, its unplug note-offs) ----
  fresh();
  static std::vector<std::vector<uint8_t>> fwd;   // what thru sends to the box
  devmidi::set_forward([](uint8_t s, uint8_t a, uint8_t b) { fwd.push_back({s, a, b}); });
  CHECK(devmidi::on_midi([](int src, uint8_t s, uint8_t a, uint8_t b) { nr::feed(src, -1, s, a, b); }));
  devmidi::plugged(0, true, "KEYSTEP");
  devmidi::plugged(1, true, "LAUNCHKEY");
  CHECK(notes(0, 1, 1, 0, nr::kAnyChan, 8) == 0);
  CHECK(notes(1, 2, 1, 1, nr::kAnyChan, 9) == 0);
  devmidi::midi(0, 0x90, 48, 100);
  devmidi::midi(1, 0x90, 50, 100);
  devmidi::midi(0, 0xF8, 0, 0);
  // thru unchanged: every message forwarded as it came, routes or not
  CHECK(fwd.size() == 3 && fwd[0] == (std::vector<uint8_t>{0x90, 48, 100}) && fwd[1] == (std::vector<uint8_t>{0x90, 50, 100}) &&
        fwd[2] == (std::vector<uint8_t>{0xF8, 0, 0}));
  CHECK(g_ev.size() == 2 && has(0, 8, 0x90, 48, 100) && has(1, 9, 0x90, 50, 100));
  g_ev.clear();
  devmidi::plugged(0, false, "");          // device 0 pulled with its key down
  CHECK(has(0, 8, 0x80, 48, 0) && nr::route(0, 0).held == 0 && nr::route(1, 0).held == 1);
  CHECK(fwd.size() == 4 && fwd[3] == (std::vector<uint8_t>{0x80, 48, 0}));   // thru's own release, as before
  // gone() covers a source whose note-offs never came (the platform's unplug path)
  g_ev.clear();
  nr::gone(1, -1);
  CHECK(g_ev.size() == 1 && has(1, 9, 0x80, 50, 0) && nr::route(1, 0).held == 0);
  // thru off: the service still gets the device (devmidi's listeners), thru gets nothing
  devmidi::set_thru(false);
  fwd.clear();
  g_ev.clear();
  devmidi::midi(1, 0x90, 52, 100);
  CHECK(fwd.empty() && g_ev.size() == 1 && has(1, 9, 0x90, 52, 100));
  devmidi::set_thru(true);
  devmidi::plugged(1, false, "");

  // ---- held notes: link reset ----
  fresh();
  CHECK(notes(0, 1, 1, 0, 0, 1) == 0);
  CHECK(notes(1, 1, 1, 0, 0, 1) == 0);
  nr::feed(0, -1, 0x90, 60, 100);
  g_ev.clear();
  nr::reset(0);
  CHECK(g_ev.size() == 1 && has(0, 1, 0x80, 60, 0));
  CHECK(!nr::route(0, 0).open && nr::route(1, 0).open && nr::route(1, 0).held == 1);   // the other box untouched

  // ---- a freed slot: held notes forgotten without note-offs, the route stays ----
  fresh();
  CHECK(notes(0, 1, 1, 0, 0, 4) == 0);
  nr::feed(0, -1, 0x90, 60, 100);
  g_ev.clear();
  nr::slot_freed(0, 4);
  CHECK(g_ev.empty() && nr::route(0, 0).open && nr::route(0, 0).held == 0);

  // ---- one delivery per slot, however many routes match; the note-off follows the note-on's route ----
  fresh();
  CHECK(notes(0, 1, 1, nr::kUsbAAny, nr::kAnyChan, 3) == 0);
  CHECK(notes(0, 2, 2, 0, 0, 3) == 1);     // the same notes again into the same slot
  CHECK(notes(0, 3, 3, 0, 0, 4) == 2);     // and into another slot
  nr::feed(0, -1, 0x90, 60, 100);
  CHECK(g_ev.size() == 2 && has(0, 3, 0x90, 60, 100) && has(0, 4, 0x90, 60, 100));
  g_ev.clear();
  nr::feed(0, -1, 0x80, 60, 0);
  CHECK(g_ev.size() == 2 && has(0, 3, 0x80, 60, 0) && has(0, 4, 0x80, 60, 0));
  CHECK(nr::route(0, 0).held == 0 && nr::route(0, 1).held == 0 && nr::route(0, 2).held == 0);
  // the route that delivered goes: its slot hears the note end, though another route still matches
  nr::feed(0, -1, 0x90, 62, 100);
  g_ev.clear();
  CHECK(send(0, {nr::kUnnote, 1, 0}));
  CHECK(g_ev.size() == 1 && has(0, 3, 0x80, 62, 0));

  // ---- refusals: full, bad fields, malformed ----
  fresh();
  for (int i = 0; i < nr::kRoutes; ++i) CHECK(notes(0, (uint8_t)i, 1, 0, 0, (uint8_t)i) == i);
  CHECK(notes(0, 20, 1, 0, 0, 0) == -nr::kFull);
  CHECK(notes(1, 21, 1, 0, 0, 0) == 0);    // box 1 has its own table
  CHECK(nr::stats(0).refused == 1 && nr::stats(0).last_refused == nr::kFull);
  fresh();
  CHECK(notes(0, 1, 1, 4, 0, 0) == -nr::kBad);       // no device 5
  CHECK(notes(0, 1, 1, 0, 16, 0) == -nr::kBad);      // channel 17
  CHECK(notes(0, 1, 1, 0, 0, nr::kSlots) == -nr::kBad);
  CHECK(!send(0, {nr::kNotes, 1, 1, 0, 0}));         // short: no answer
  CHECK(take(0).empty() && nr::stats(0).ops_bad == 1);
  CHECK(!send(2, {nr::kNotes, 1, 1, 0, 0, 0}));      // no box 2
  CHECK(!send(0, {0x20, 1, 1, 2, 4, 0, 0}));         // a route (audio) op is not ours
  CHECK(nr::stats(0).open == 0);

  // ---- 2B DEVICES, and 6A unasked after a plug ----
  fresh();
  g_dev[0] = "KEYSTEP";
  g_dev[2] = "A VERY LONG KEYBOARD NAME THAT IS CUT";
  CHECK(send(1, {nr::kDevices, 33}));
  auto a = take(1);
  CHECK(a.size() >= 10 && a[6] == nr::kDeviceList && a[7] == 33 && a[8] == 2 && a.back() == 0xF7);
  CHECK(a[9] == 0 && a[10] == 7 && std::string(a.begin() + 11, a.begin() + 18) == "KEYSTEP");
  CHECK(a[18] == 2 && a[19] == nr::kNameLen && std::string(a.begin() + 20, a.begin() + 20 + nr::kNameLen) == "A VERY LONG KEYBOARD NAM");
  CHECK((int)a.size() == 20 + nr::kNameLen + 1);
  for (uint8_t b : a) CHECK(b == 0xF0 || b == 0xF7 || b < 0x80);
  CHECK(take(0).empty());
  nr::plugged();
  for (int bx = 0; bx < nr::kBoxes; ++bx) {
    auto u = take(bx);
    CHECK(u.size() > 9 && u[6] == nr::kDeviceList && u[7] == nr::kUnasked && u[8] == 2);
  }
  g_dev[0] = g_dev[2] = "";
  CHECK(send(0, {nr::kDevices, 1}));
  a = take(0);
  CHECK(a.size() == 10 && a[8] == 0);

  printf("test_noteroute: ok\n");
  return 0;
}
