// pi/engine/tests/test_usbmidi_os_path.cpp - usbmidi_os_path.h: the computer's MIDI on its way to the Rytm and the
// usbmidi=os: decisions the gadget takes from it (sidecar_gadget.cpp).
#include <stdio.h>

#include <vector>

#include "check.h"
#include "usbmidi_os_path.h"
#include "usbmidi_pack.h"

using Bytes = std::vector<uint8_t>;
using Path = OwnedToRytmPath<2048>;
using usbmidi_os::kEmu;
using usbmidi_os::kOff;
using usbmidi_os::kOn;

static Bytes Pack(const Bytes& m) {     // one message as event packets on cable 0
  Bytes out;
  if (m[0] == 0xF0) {
    out.resize(usbmidi_sysex_bytes((int)m.size()));
    out.resize(usbmidi_pack_sysex(m.data(), (int)m.size(), out.data()));
    return out;
  }
  uint8_t cin = m[0] >= 0xF8 ? 0xF : m[0] >> 4;
  return {cin, m[0], m.size() > 1 ? m[1] : (uint8_t)0, m.size() > 2 ? m[2] : (uint8_t)0};
}
static void Feed(Path& p, const Bytes& packets, size_t from = 0, size_t to = ~(size_t)0) {
  for (size_t i = from; i < packets.size() && i < to; i += 4) CHECK(p.FromHost(&packets[i]));
}
static Bytes Drain(Path& p) {           // what the loop would send, as packets
  Bytes out;
  uint32_t w;
  while (p.Pop(&w)) {
    uint8_t b[4];
    memcpy(b, &w, 4);
    out.insert(out.end(), b, b + 4);
  }
  return out;
}
static bool HasSysExData(const Bytes& packets) {   // any SysEx packet (start, continuation or end)
  for (size_t i = 0; i < packets.size(); i += 4) {
    uint8_t cin = packets[i] & 0x0F;
    if (cin == 0x4 || cin == 0x6 || cin == 0x7 || (cin == 0x5 && packets[i + 1] == 0xF7)) return true;
  }
  return false;
}
// the gadget's step: the gate decides, the path is cut if told, the filter follows the pass
static void Apply(Path& p, const OsGate& g, bool cut) {
  if (cut) p.Cut();
  p.Filter().SetPassOsUpdate(g.Pass());
}
static Bytes OsMsg(uint8_t cmd, int len, uint8_t first = 0) {   // an OS .syx message's shape (device byte 0C), len
  Bytes m = {0xF0, 0x00, 0x20, 0x3C, 0x0C, 0x00, cmd};          // data bytes; first: the start's 01, the end's 02
  for (int i = 0; i < len; i++) m.push_back(i == 0 && first ? first : (uint8_t)(i * 7 & 0x7F));
  m.push_back(0xF7);
  return m;
}

int main() {
  const Bytes note = {0x90, 60, 100}, start = OsMsg(0x7F, 4, 0x01), data = OsMsg(0x7E, 300), end = OsMsg(0x7F, 4, 0x02);

  // H2: usbmidi=os:emu, the emulator the host, an upgrade under way; link=usb in the middle of it: nothing of the
  // image - queued, the message being filtered, or what the computer sends after - reaches the real Rytm
  {
    Path p;
    OsGate g;
    Apply(p, g, g.SetMode(kEmu));
    CHECK(!g.Pass());
    Apply(p, g, g.SetLink(true));
    CHECK(g.Pass() && p.Filter().PassOsUpdate());
    Bytes s = Pack(start), d = Pack(data);
    Feed(p, s);
    for (int k = 0; k < 4; k++) Feed(p, d);
    Feed(p, d, 0, d.size() / 8 * 4);                   // half a message: the filter is passing it
    CHECK(p.Size() > 0);
    uint32_t w;
    for (int k = 0; k < 10; k++) CHECK(p.Pop(&w));   // the emulator took some: a SysEx is half sent
    CHECK(p.InSysEx());
    bool cut = g.SetLink(false);                   // link=usb
    CHECK(cut);
    Apply(p, g, cut);
    CHECK(!g.Pass() && !p.Filter().PassOsUpdate());
    CHECK(p.Size() == 0 && !p.InSysEx());
    CHECK(p.Filter().InSysEx());                   // dropping the rest of the cut message
    CHECK(p.CutPackets() > 0);
    Feed(p, d, d.size() / 8 * 4);                      // the rest of the half message
    Feed(p, s);                                    // and more of the upgrade
    Feed(p, d);
    Feed(p, Pack(note));
    Bytes out = Drain(p);
    CHECK(out == Pack(note));                      // only the note: no start, no continuation, no end
    CHECK(!HasSysExData(out));
    CHECK(!p.InSysEx());
  }
  // usbmidi=os:on: a host switch either way cuts too (the queue was meant for the other destination)
  {
    Path p;
    OsGate g;
    Apply(p, g, g.SetMode(kOn));
    CHECK(g.Pass());
    bool cut = g.SetLink(true);
    CHECK(cut && g.Pass());
    Feed(p, Pack(start));
    Feed(p, Pack(data), 0, 40);
    Apply(p, g, cut = g.SetLink(false));
    CHECK(cut && g.Pass());
    CHECK(p.Size() == 0);
    Feed(p, Pack(data), 40);                       // the rest of the cut message: dropped up to its end
    CHECK(Drain(p).empty());
    Feed(p, Pack(start));                          // a new message passes again (the setting is on)
    CHECK(Drain(p) == Pack(start));
  }
  // M-a: usbmidi=os:on, an upgrade to the emulator under way, link=usb: the switch aborts it, so the rest of the image
  // and its end never reach the real Rytm (the pass would stay on with on); counted; 3 s of quiet bring the pass back
  for (usbmidi_os::Mode mode : {kOn, kEmu}) {
    Path p;
    OsGate g;
    Apply(p, g, g.SetMode(mode));
    Apply(p, g, g.SetLink(true));
    uint32_t t = 100;
    Feed(p, Pack(start));
    Feed(p, Pack(data));
    Drain(p);                                      // the emulator took them
    g.Seen(p.Filter().OsMessages(), t);
    Feed(p, Pack(data), 0, 40);
    t += 1000;
    bool cut = g.SetLink(false);
    CHECK(cut);
    CHECK(g.Switched(cut, p.Filter().OsMessages(), t));
    CHECK(g.Aborted() && !g.Pass() && g.Aborts() == 1);
    Apply(p, g, cut);
    Feed(p, Pack(data), 40);                       // the rest of the message under way
    Feed(p, Pack(data));                           // the rest of the image
    Feed(p, Pack(end));                            // its end (7F 02)
    Feed(p, Pack(note));
    CHECK(Drain(p) == Pack(note));                 // only the note reaches the real Rytm
    CHECK(!g.Switched(g.SetLink(true), p.Filter().OsMessages(), t));   // aborted already: counted once
    CHECK(g.Aborts() == 1);
    // no upgrade under way: a switch only cuts (as before), no abort
    OsGate h;
    h.SetMode(mode);
    CHECK(!h.Switched(h.SetLink(true), 0, 50));
    CHECK(!h.Aborted() && h.Aborts() == 0);
  }
  // usbmidi=os:off: a host switch cuts nothing (dev's behaviour); the same switch twice is no switch
  {
    OsGate g;
    CHECK(!g.SetLink(true));
    CHECK(!g.SetLink(true));
    CHECK(!g.SetLink(false));
    Path p;
    Feed(p, Pack(note));
    Apply(p, g, g.SetLink(true));
    CHECK(Drain(p) == Pack(note));
  }
  // the setting turned off (or emu while USB is the host) during an upgrade: cut, the pass off
  {
    for (usbmidi_os::Mode to : {kOff, kEmu}) {
      Path p;
      OsGate g;
      Apply(p, g, g.SetMode(kOn));
      Feed(p, Pack(start));
      Feed(p, Pack(data), 0, 40);
      bool cut = g.SetMode(to);
      CHECK(cut);
      Apply(p, g, cut);
      CHECK(!p.Filter().PassOsUpdate() && p.Size() == 0);
      Feed(p, Pack(data), 40);
      Feed(p, Pack(data));
      CHECK(Drain(p).empty());
    }
  }
  // flow control: held while on and without room for a whole transfer (128 packets and what the filter may hold)
  {
    Path p;
    OsGate g;
    CHECK(!g.Hold(false));                          // off: never held (dev)
    g.SetMode(kOn);
    CHECK(p.Room(128));
    CHECK(!g.Hold(p.Room(128)));
    uint8_t clk[4] = {0x0F, 0xF8, 0, 0};
    while (p.Size() + 128 + MidiFilter::kHoldPackets <= 2048) CHECK(p.FromHost(clk));
    CHECK(!p.Room(128));
    CHECK(g.Hold(p.Room(128)));
    CHECK(!g.Resume(true, p.Room(128)));
    uint32_t w;
    CHECK(p.Pop(&w));
    CHECK(p.Room(128));
    CHECK(g.Resume(true, p.Room(128)));
    CHECK(!g.Resume(false, p.Room(128)));          // not held: nothing to re-arm
  }
  // M1: the setting turned off while the endpoint is held and the queue still full: re-armed at once, whatever the room
  // (before: Resume only while on, so the computer's MIDI OUT stayed NAKed until a USB reset)
  for (usbmidi_os::Mode from : {kOn, kEmu}) {
    OsGate g;
    g.SetMode(from);
    CHECK(g.Hold(false));
    CHECK(!g.Resume(true, false));
    g.SetMode(kOff);
    CHECK(g.Resume(true, false));
    CHECK(!g.Hold(false));                          // and never held again while off
  }
  // M2: the TCP link's engine records wait behind the computer's half-sent SysEx only while the setting is on. Off: a
  // half-sent state left from before a switch (nothing pops the queue under link=tcp then) never holds them back
  {
    OsGate g;
    CHECK(!g.EngineWaits(true));
    CHECK(!g.EngineWaits(false));
    g.SetMode(kOn);
    CHECK(g.EngineWaits(true));
    CHECK(!g.EngineWaits(false));
    g.SetMode(kEmu);
    CHECK(g.EngineWaits(true));
    g.SetMode(kOff);
    CHECK(!g.EngineWaits(true));
    // the frozen state the review describes: the computer's kit dump half sent, then the setting off
    Bytes kit = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0x52, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0xF7};
    Path q;
    Feed(q, Pack(kit), 0, 20);                      // 5 of its 6 packets: decided, passing
    uint32_t w;
    while (q.Pop(&w)) {}
    CHECK(q.InSysEx());                             // half sent
    CHECK(!g.EngineWaits(q.InSysEx()));             // off: the engine's records go anyway
  }
  // M5: a failed send during an upgrade aborts it: the path cut, OS messages dropped (never the rest of the image with a
  // hole in it), loud; the pass back once the computer has sent no OS message for kQuietUs
  {
    Path p;
    OsGate g;
    Apply(p, g, g.SetMode(kOn));
    uint32_t t = 1000;
    CHECK(!g.Lost(true, false, p.Filter().OsMessages(), t));   // no upgrade seen: a failed send of notes aborts nothing
    Feed(p, Pack(start));
    for (int k = 0; k < 3; k++) Feed(p, Pack(data));
    CHECK(p.Filter().OsMessages() == 4);
    g.Seen(p.Filter().OsMessages(), t);
    uint32_t w;
    for (int k = 0; k < 5; k++) CHECK(p.Pop(&w));   // the merge took a chunk; its send fails
    t += 1000;
    bool lost = g.Lost(true, p.Busy(), p.Filter().OsMessages(), t);
    CHECK(lost && g.Aborted() && !g.Pass() && g.Aborts() == 1);
    Apply(p, g, lost);
    CHECK(p.Size() == 0 && !p.Filter().PassOsUpdate());
    CHECK(!g.Lost(false, true, p.Filter().OsMessages(), t));   // the Rytm's MIDI then down too: one abort, not two
    // the computer goes on sending the image: all dropped, and it keeps the pass off
    for (int k = 0; k < 5; k++) {
      t += 500000;
      Feed(p, Pack(data));
      CHECK(!g.Quiet(p.Filter().OsMessages(), t));
    }
    Feed(p, Pack(note));
    CHECK(Drain(p) == Pack(note));                 // its notes still go
    t += OsGate::kQuietUs - 1;
    CHECK(!g.Quiet(p.Filter().OsMessages(), t));
    t += 1;
    CHECK(g.Quiet(p.Filter().OsMessages(), t));    // quiet for kQuietUs: the pass is back
    p.Filter().AwaitStart(true);                   // as the gadget does: from a new start
    Apply(p, g, false);
    CHECK(g.Pass() && p.Filter().PassOsUpdate() && !g.Aborted());
    Feed(p, Pack(start));                          // a new upgrade goes through
    CHECK(Drain(p) == Pack(start));
  }
  // the aborted image's tail: a sender pausing longer than kQuietUs mid-image, then going on, never gets its tail
  // through: after the quiet the filter waits for a new start (image 7F or the RPC's 50)
  {
    Path p;
    OsGate g;
    Apply(p, g, g.SetMode(kOn));
    uint32_t t = 1000;
    Feed(p, Pack(start));
    Feed(p, Pack(data));
    g.Seen(p.Filter().OsMessages(), t);
    Drain(p);
    Feed(p, Pack(data));
    bool lost = g.Lost(true, p.Busy(), p.Filter().OsMessages(), t + 10);
    CHECK(lost);
    Apply(p, g, lost);
    t += OsGate::kQuietUs + 100;                   // the sender pauses
    CHECK(g.Quiet(p.Filter().OsMessages(), t));
    p.Filter().AwaitStart(true);
    Apply(p, g, false);
    CHECK(g.Pass());
    for (int k = 0; k < 3; k++) Feed(p, Pack(data));   // and goes on with the image's tail
    Feed(p, Pack(end));                            // and its end (7F 02): not a start, dropped too
    CHECK(Drain(p).empty());
    CHECK(p.Filter().AwaitingStart());
    Feed(p, Pack(note));
    CHECK(Drain(p) == Pack(note));                 // other MIDI as ever
    Feed(p, Pack(start));                          // a new upgrade: its start ends the wait
    Feed(p, Pack(data));
    Bytes want = Pack(start), d = Pack(data);
    want.insert(want.end(), d.begin(), d.end());
    CHECK(Drain(p) == want);
    CHECK(!p.Filter().AwaitingStart());
    // the RPC's OsUpgradeStart ends the wait too; its Write does not
    Path q;
    q.Filter().SetPassOsUpdate(true);
    q.Filter().AwaitStart(true);
    Bytes w51 = {0xF0, 0x00, 0x20, 0x3C, 0x10, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x51, 0x01, 0xF7};
    Bytes w50 = w51;
    w50[11] = 0x50;
    Feed(q, Pack(w51));
    CHECK(Drain(q).empty());
    Feed(q, Pack(w50));
    Feed(q, Pack(w51));
    Bytes wq = Pack(w50), w1 = Pack(w51);
    wq.insert(wq.end(), w1.begin(), w1.end());
    CHECK(Drain(q) == wq);
  }
  // the clock wraps (32-bit us, ~71.6 min): the window works across the wrap, and an old sighting never comes back
  // a wrap period later (the loop calls Quiet, so Seen, every pass)
  {
    OsGate g;
    g.SetMode(kOn);
    const uint32_t t0 = 0xFFFFFFFFu - 1000000u;    // 1 s before the wrap
    g.Seen(1, t0);
    CHECK(g.Upgrading(t0));
    CHECK(g.Upgrading(t0 + 2000000u));              // 1 s after the wrap: still within 3 s
    CHECK(!g.Upgrading(t0 + OsGate::kQuietUs));
    for (uint64_t t = t0; t < (uint64_t)t0 + 0x100000000ull + 2000000u; t += 500000u) {
      CHECK(!g.Quiet(1, (uint32_t)t));              // the loop, every 0.5 s, for a whole wrap period and more
      bool in = t - t0 < OsGate::kQuietUs;
      CHECK(g.Upgrading((uint32_t)t) == in);        // never again after the first 3 s
    }
    CHECK(!g.Lost(true, true, 1, t0 + 1000000u));   // a wrap period later, at the old time: no spurious abort
    CHECK(g.Aborts() == 0);
    // an abort across the wrap: quiet comes 3 s after the last OS message, wrap or not
    g.Seen(2, t0 + 500000u);
    CHECK(g.Lost(true, false, 2, t0 + 600000u));
    CHECK(!g.Quiet(2, t0 + 2500000u));             // past the wrap, 2 s after
    CHECK(g.Quiet(2, t0 + 500000u + OsGate::kQuietUs));
  }
  // M5: the Rytm leaves right after a finished upgrade (it reboots): nothing on the way, no abort; with data still
  // waiting, an abort. A new setting clears an abort.
  {
    Path p;
    OsGate g;
    Apply(p, g, g.SetMode(kOn));
    uint32_t t = 5000;
    Feed(p, Pack(start));
    Feed(p, Pack(data));
    Feed(p, Pack(end));                            // the end message
    Drain(p);
    CHECK(!p.Busy());
    CHECK(!g.Lost(false, p.Busy(), p.Filter().OsMessages(), t));
    Feed(p, Pack(data));
    CHECK(p.Busy());
    CHECK(g.Lost(false, p.Busy(), p.Filter().OsMessages(), t + 10));
    CHECK(!g.Pass());
    Apply(p, g, g.SetMode(kOn));
    CHECK(g.Pass() && !g.Aborted());
    // off or emu over USB: no pass, so nothing to abort
    OsGate h;
    h.SetMode(kEmu);
    h.Seen(1, 0);
    CHECK(!h.Lost(true, true, 2, 10));
  }
  printf("test_usbmidi_os_path: PASS\n");
  return 0;
}
