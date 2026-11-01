// pi/engine/tests/test_usb_box.cpp - which USB MIDI device is the host link: the strings as the firmware images and a real Analog Four MKII give them.
#include "check.h"
#include "usb_box.h"
#include "hello_gate.h"

int main() {
  const char* v = "Elektron Music Machines";
  CHECK(usb_box(v, "Elektron Analog Rytm MKII") == UsbBoxHost);
  CHECK(usb_box(v, "Elektron Analog Rytm") == UsbBoxHost);
  CHECK(usb_box(v, "Elektron Digitakt") == UsbBoxHost);
  CHECK(usb_box(0, 0) == UsbBoxHost);   // no strings: as before
  CHECK(usb_box("", "") == UsbBoxHost);
  CHECK(usb_box(0, "Analog Rytm MKII MIDI") == UsbBoxHost);
  // what the engine's replies must not reach
  CHECK(usb_box(v, "Elektron Analog Four MKII") == UsbBoxHello);
  CHECK(usb_box(v, "Elektron Analog Keys") == UsbBoxOther);
  CHECK(usb_box(v, "Elektron Analog Four") == UsbBoxOther);
  CHECK(usb_box(v, "Elektron Digitakt II") == UsbBoxOther);
  CHECK(usb_box(v, "Elektron Syntakt") == UsbBoxOther);
  CHECK(usb_box(v, 0) == UsbBoxOther);  // an Elektron box of no known name is not the Rytm
  CHECK(usb_box(v, "") == UsbBoxOther);
  CHECK(usb_box(0, "Elektron Analog Four MKII") == UsbBoxHello);
  // everybody else stays a keyboard
  CHECK(usb_box("Arturia", "KeyStep") == UsbBoxKeyboard);
  CHECK(usb_box(0, "USB MIDI") == UsbBoxKeyboard);
  CHECK(usb_box("Somebody", 0) == UsbBoxKeyboard);
  {  // the gate before an A4 is the box in use: whole hellos, nothing else
    HelloGate g;
    const unsigned char hello[] = {0xF0, 0x7D, 0x52, 0x54, 0x4F, 1, 0, 3, 1, 'A', '4', 0xF7};
    const unsigned char noise[] = {0x90, 60, 100, 0xF8, 0xF0, 0x00, 0x20, 0x3C, 0x06, 0x00, 0x7F, 0xF7, 0xF0, 0x7D, 0x52, 0x54, 0x04, 0, 60, 100, 0xF7};
    unsigned got = 0;
    for (unsigned char b : noise) got += g.feed(b);
    CHECK(got == 0);
    for (unsigned k = 0; k < sizeof hello; ++k) got = g.feed(hello[k]);
    CHECK(got == sizeof hello && !memcmp(g.data(), hello, got));
    for (unsigned k = 0; k < sizeof hello - 1; ++k) got = g.feed(hello[k]);   // a clock inside it, then a cut
    CHECK(g.feed(0xF8) == 0 && g.feed(0x90) == 0 && g.feed(0xF7) == 0);
    for (int k = 0; k < 100; ++k) g.feed(k == 0 ? 0xF0 : 0x11);               // too long: dropped, no overrun
    CHECK(g.feed(0xF7) == 0);
  }
  printf("test_usb_box: ok\n");
  return 0;
}
