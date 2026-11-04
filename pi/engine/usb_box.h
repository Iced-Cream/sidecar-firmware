// pi/engine/usb_box.h - what a USB MIDI device on the USB-A ports is to the engine, from its USB strings (Circle's umidiN carries them; its USB id is
// not on that object). The host link is the Rytm's, or a box with a device profile that says hello: the engine's replies go there and nowhere else, so
// the product decides. The vendor alone does not: every Elektron box says "Elektron Music Machines", and an Analog Four's MIDI taken for the Rytm's gets
// the Rytm's records. Portable: tests/test_usb_box.cpp.
#pragma once
#include <string.h>

enum TUsbBox {
  UsbBoxHost,       // the host link
  UsbBoxKeyboard,   // somebody's keyboard or interface: notes play the Rytm's selected track, the MIDI machine reaches it
  UsbBoxOther,      // an Elektron box that is neither: left alone
  UsbBoxHello,      // an Analog Four MKII: nothing goes to it, its hello is heard (hello_gate.h); once it is the box in use, the host link
};

inline TUsbBox usb_box(const char* vendor, const char* product) {
  bool has_v = vendor != 0 && *vendor != 0, has_p = product != 0 && *product != 0;
  if (!has_v && !has_p) return UsbBoxHost;   // a device without strings: taken for the Rytm, as ever
  if (has_p && strstr(product, "Rytm") != 0) return UsbBoxHost;
  if (has_p && strstr(product, "Digitakt") != 0 && strstr(product, "Digitakt II") == 0) return UsbBoxHost;
  if (has_p && strstr(product, "Analog Four MKII") != 0) return UsbBoxHello;
  bool elektron = (has_v && strstr(vendor, "Elektron") != 0) || (has_p && strstr(product, "Elektron") != 0);
  return elektron ? UsbBoxOther : UsbBoxKeyboard;
}
