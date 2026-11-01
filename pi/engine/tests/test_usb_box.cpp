// pi/engine/tests/test_usb_box.cpp - which USB MIDI device is the host link: the strings as the firmware images and a real Analog Four MKII give them.
#include "check.h"
#include "usb_box.h"

int main() {
  const char* v = "Elektron Music Machines";
  CHECK(usb_box(v, "Elektron Analog Rytm MKII") == UsbBoxHost);
  CHECK(usb_box(v, "Elektron Analog Rytm") == UsbBoxHost);
  CHECK(usb_box(v, "Elektron Digitakt") == UsbBoxHost);
  CHECK(usb_box(0, 0) == UsbBoxHost);   // no strings: as before
  CHECK(usb_box("", "") == UsbBoxHost);
  CHECK(usb_box(0, "Analog Rytm MKII MIDI") == UsbBoxHost);
  // what the engine's replies must not reach
  CHECK(usb_box(v, "Elektron Analog Four MKII") == UsbBoxOther);
  CHECK(usb_box(v, "Elektron Analog Keys") == UsbBoxOther);
  CHECK(usb_box(v, "Elektron Analog Four") == UsbBoxOther);
  CHECK(usb_box(v, "Elektron Digitakt II") == UsbBoxOther);
  CHECK(usb_box(v, "Elektron Syntakt") == UsbBoxOther);
  CHECK(usb_box(v, 0) == UsbBoxOther);  // an Elektron box of no known name is not the Rytm
  CHECK(usb_box(v, "") == UsbBoxOther);
  CHECK(usb_box(0, "Elektron Analog Four MKII") == UsbBoxOther);
  // everybody else stays a keyboard
  CHECK(usb_box("Arturia", "KeyStep") == UsbBoxKeyboard);
  CHECK(usb_box(0, "USB MIDI") == UsbBoxKeyboard);
  CHECK(usb_box("Somebody", 0) == UsbBoxKeyboard);
  printf("test_usb_box: ok\n");
  return 0;
}
