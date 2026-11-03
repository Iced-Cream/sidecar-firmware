// pi/engine/tests/test_usbmidi_os_path.cpp - usbmidi_os_path.h: the computer's MIDI on its way to the Rytm and the
// usbmidi=os: decisions the gadget takes from it (sidecar_gadget.cpp).
#include <stdio.h>

#include <vector>

#include "check.h"
#include "usbmidi_os_path.h"
#include "usbmidi_pack.h"

using Bytes = std::vector<uint8_t>;
using Path = ToRytmPath<2048>;
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
static Bytes OsMsg(uint8_t cmd, int len) {   // an OS .syx message's shape (device byte 0C), len data bytes
  Bytes m = {0xF0, 0x00, 0x20, 0x3C, 0x0C, 0x00, cmd};
  for (int i = 0; i < len; i++) m.push_back((uint8_t)(i * 7 & 0x7F));
  m.push_back(0xF7);
  return m;
}

int main() {
  const Bytes note = {0x90, 60, 100}, start = OsMsg(0x7F, 4), data = OsMsg(0x7E, 300);

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
  // flow control: held while on and without room for a whole transfer (128 packets and the filter's 3)
  {
    Path p;
    OsGate g;
    CHECK(!g.Hold(false));                          // off: never held (dev)
    g.SetMode(kOn);
    CHECK(p.Room(128));
    CHECK(!g.Hold(p.Room(128)));
    uint8_t clk[4] = {0x0F, 0xF8, 0, 0};
    while (p.Size() + 128 + 3 <= 2048) CHECK(p.FromHost(clk));
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
    Feed(q, Pack(kit), 0, 16);
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
    Apply(p, g, false);
    CHECK(g.Pass() && p.Filter().PassOsUpdate() && !g.Aborted());
    Feed(p, Pack(start));                          // a new upgrade goes through
    CHECK(Drain(p) == Pack(start));
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
    Feed(p, Pack(start));                          // the end message
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
