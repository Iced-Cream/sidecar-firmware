// pi/engine/tests/test_usbmidi_os.cpp - usbmidi_os.h: usbmidi=os:on|off|emu, the osupdate= alias, cmdline precedence.
#include <stdio.h>

#include "check.h"
#include "usbmidi_os.h"

using namespace usbmidi_os;

int main() {
  CHECK(parse_usbmidi("os:on") == kOn);
  CHECK(parse_usbmidi("os:off") == kOff);
  CHECK(parse_usbmidi("os:emu") == kEmu);
  CHECK(parse_usbmidi("on") == kInvalid);
  CHECK(parse_usbmidi("os:") == kInvalid);
  CHECK(parse_usbmidi("os:rytm") == kInvalid);
  CHECK(parse_usbmidi("data:on") == kInvalid);
  CHECK(parse_usbmidi(nullptr) == kInvalid);

  // the alias keeps v2-v5's words
  CHECK(parse_osupdate("rytm") == kOn);
  CHECK(parse_osupdate("1") == kEmu);
  CHECK(parse_osupdate("emu") == kEmu);
  CHECK(parse_osupdate("0") == kOff);
  CHECK(parse_osupdate("off") == kOff);
  CHECK(parse_osupdate("on") == kInvalid);

  bool known;
  CHECK(parse_command("usbmidi=os:on", &known) == kOn && known);
  CHECK(parse_command("osupdate=rytm", &known) == kOn && known);
  CHECK(parse_command("usbmidi=os:maybe", &known) == kInvalid && known);
  CHECK(parse_command("midibridge=os:on", &known) == kInvalid && !known);	// the bridge's own switch: not ours
  CHECK(parse_command("link=usb", &known) == kInvalid && !known);

  CHECK(from_cmdline(nullptr, nullptr) == kOff);
  CHECK(from_cmdline("os:on", nullptr) == kOn);
  CHECK(from_cmdline(nullptr, "rytm") == kOn);			// the v4/v5 rpiboot cmdline still works
  CHECK(from_cmdline("os:off", "rytm") == kOff);		// usbmidi= wins
  CHECK(from_cmdline("bad", "1") == kEmu);			// a bad usbmidi= falls back to the alias
  CHECK(from_cmdline("bad", nullptr) == kOff);

  CHECK(strcmp(name(kOn), "on") == 0 && strcmp(name(kEmu), "emu") == 0 && strcmp(name(kOff), "off") == 0);

  printf("test_usbmidi_os: PASS\n");
  return 0;
}
