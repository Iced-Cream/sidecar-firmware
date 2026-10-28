// pi/engine/tests/test_reboot_sysex.cpp - reboot_sysex.h: the exact request matches, in any packet split and with real
// time inside; a longer, shorter or different SysEx, or one cut by a status byte, does not.
#include <stdio.h>

#include <vector>

#include "check.h"
#include "reboot_sysex.h"
#include "usbmidi_pack.h"
#include "usbmidi_stream.h"

using Bytes = std::vector<uint8_t>;

static bool Match(const Bytes& stream) {
  UsbMidiFromStream s;
  RebootSysEx r;
  for (uint8_t b : stream) s.Feed(b, [&](const uint8_t* p) { r.Feed(p); });
  return r.Matched();
}

int main() {
  const Bytes req(RebootSysEx::Magic(), RebootSysEx::Magic() + RebootSysEx::kLen);
  CHECK(Match(req));

  Bytes clock = req;                     // a clock byte inside the SysEx
  clock.insert(clock.begin() + 5, 0xF8);
  CHECK(Match(clock));

  Bytes after = {0x90, 60, 100};         // a note first, then the request
  after.insert(after.end(), req.begin(), req.end());
  CHECK(Match(after));

  Bytes longer = req;                    // one byte more before F7
  longer.insert(longer.end() - 1, 0x00);
  CHECK(!Match(longer));

  Bytes shorter = req;                   // "REBOO"
  shorter.erase(shorter.end() - 2);
  CHECK(!Match(shorter));

  Bytes other = req;                     // another host-link command
  other[4] = 0x04;
  CHECK(!Match(other));

  Bytes cut(req.begin(), req.begin() + 6);   // cut by a note, the rest after
  cut.insert(cut.end(), {0x90, 60, 100});
  cut.insert(cut.end(), req.begin() + 6, req.end());
  CHECK(!Match(cut));

  Bytes restart(req.begin(), req.begin() + 7);   // an unfinished one, then the whole request
  restart.insert(restart.end(), req.begin(), req.end());
  CHECK(Match(restart));

  printf("test_reboot_sysex: PASS\n");
  return 0;
}
