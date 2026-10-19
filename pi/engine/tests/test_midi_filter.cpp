// pi/engine/tests/test_midi_filter.cpp - midi_filter.h: what passes, what is dropped, order kept.
#include <stdio.h>

#include <vector>

#include "check.h"
#include "midi_filter.h"
#include "usbmidi_pack.h"

using Bytes = std::vector<uint8_t>;

static Bytes Pack(const Bytes& m) {     // one message as event packets on cable 0
  Bytes out;
  if (m[0] == 0xF0) {
    out.resize(usbmidi_sysex_bytes((int)m.size()));
    out.resize(usbmidi_pack_sysex(m.data(), (int)m.size(), out.data()));
    return out;
  }
  uint8_t cin = m[0] >= 0xF8 ? 0xF : m[0] >> 4;
  out = {cin, m[0], m.size() > 1 ? m[1] : (uint8_t)0, m.size() > 2 ? m[2] : (uint8_t)0};
  return out;
}

static Bytes Run(MidiFilter& f, const Bytes& packets) {
  Bytes out;
  for (size_t i = 0; i < packets.size(); i += 4)
    f.Feed(&packets[i], [&](const uint8_t* p) { out.insert(out.end(), p, p + 4); });
  return out;
}

int main() {
  const Bytes note = {0x90, 60, 100}, cc = {0xB3, 7, 99}, clock = {0xF8};
  const Bytes kit = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0x52, 1, 2, 3, 4, 5, 6, 7, 0xF7};   // an Elektron kit dump
  const Bytes os = {0xF0, 0x00, 0x20, 0x3C, 0x0C, 0x00, 0x7E, 0, 1, 2, 3, 0xF7};            // an OS .syx message
  const Bytes os_start = {0xF0, 0x00, 0x20, 0x3C, 0x0C, 0x00, 0x7F, 0x01, 0xF7};
  const Bytes link = {0xF0, 0x7D, 0x52, 0x54, 0x04, 0x00, 48, 100, 0xF7};
  const Bytes ci = {0xF0, 0x7E, 0x7F, 0x0D, 0x70, 0x02, 1, 2, 3, 4, 5, 6, 7, 8, 0xF7};
  const Bytes tiny = {0xF0, 0xF7}, short3 = {0xF0, 0x01, 0xF7};
  const Bytes uni = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};   // identity request: universal, not CI

  // to the Rytm: the kit, notes, CCs, clock, tiny SysEx and non-CI universal pass; OS update, host link, CI do not
  {
    MidiFilter f(true);
    Bytes in, want;
    for (const Bytes* m : {&note, &kit, &os, &cc, &link, &ci, &clock, &os_start, &tiny, &short3, &uni, &note}) {
      Bytes p = Pack(*m);
      in.insert(in.end(), p.begin(), p.end());
      if (m != &os && m != &link && m != &ci && m != &os_start && m != &tiny) want.insert(want.end(), p.begin(), p.end());
    }
    Bytes out = Run(f, in);
    CHECK(out == want);
    CHECK(f.DroppedMessages() == 5);
    CHECK(!f.InSysEx());
  }
  // to the computer: the OS-update shape passes (the Rytm never sends one; only the Rytm-bound side blocks it)
  {
    MidiFilter f(false);
    Bytes in = Pack(os), l = Pack(link), c = Pack(ci), n = Pack(note);
    in.insert(in.end(), l.begin(), l.end());
    in.insert(in.end(), c.begin(), c.end());
    in.insert(in.end(), n.begin(), n.end());
    Bytes want = Pack(os);
    want.insert(want.end(), n.begin(), n.end());
    CHECK(Run(f, in) == want);
    CHECK(f.DroppedMessages() == 2);
  }
  // real-time bytes inside a held, a passing and a dropped SysEx go through at once; the SysEx stays whole
  {
    MidiFilter f(true);
    Bytes k = Pack(kit), c = Pack(clock), o = Pack(os), in, want;
    in.insert(in.end(), k.begin(), k.begin() + 4);      // kit packet 1 (held)
    in.insert(in.end(), c.begin(), c.end());            // clock: out first
    in.insert(in.end(), k.begin() + 4, k.end());        // the kit's rest: decides at packet 3
    in.insert(in.end(), o.begin(), o.begin() + 12);     // OS: 3 packets decide (dropped)
    in.insert(in.end(), c.begin(), c.end());
    in.insert(in.end(), o.begin() + 12, o.end());
    want = c;
    want.insert(want.end(), k.begin(), k.end());
    want.insert(want.end(), c.begin(), c.end());
    CHECK(Run(f, in) == want);
  }
  // a long SysEx that passes: every packet, in order
  {
    MidiFilter f(true);
    Bytes big = {0xF0, 0x00, 0x20, 0x3C};
    for (int i = 0; i < 3000; i++) big.push_back((uint8_t)(i & 0x7F));
    big.push_back(0xF7);
    Bytes p = Pack(big);
    CHECK(Run(f, p) == p);
  }
  printf("test_midi_filter: PASS\n");
  return 0;
}
