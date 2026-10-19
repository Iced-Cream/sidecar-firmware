// rytm-teensy: first hardware test. The Teensy 4.1 as USB host on its 5-pin
// USB host header, with the Analog Rytm MKII plugged in there; the Teensy's own
// micro-USB goes to the Mac for power and the serial monitor.
//
// Prints what enumerates on the host port and every MIDI message the Rytm sends.
// Serial commands (Arduino serial monitor, 115200, send a line):
//   b   send a BD trig to the Rytm (note 0 on the auto channel 14)
//   c   send CC 15 = 44 on channel 1 (BD -> EXT, on a rytm-mods 0021 build)
//
// Uses PJRC's USBHost_t36 (bundled with Teensyduino). MIDIDevice_BigBuffer takes
// 512-byte high-speed packets; the Rytm MKII is a high-speed device.
#include <USBHost_t36.h>

USBHost myusb;
USBHub hub1(myusb);
MIDIDevice_BigBuffer midi1(myusb);

static bool attached = false;
static uint32_t messages = 0, sysexBytes = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 4000) {}
  Serial.println("rytm-teensy usbhost_probe: plug the Rytm into the USB host header");
  myusb.begin();
}

static void printDevice() {
  const uint8_t *m = midi1.manufacturer(), *p = midi1.product(), *s = midi1.serialNumber();
  Serial.printf("MIDI device attached: VID %04X PID %04X  manufacturer '%s' product '%s' serial '%s'\n",
                midi1.idVendor(), midi1.idProduct(), m ? (const char *)m : "", p ? (const char *)p : "",
                s ? (const char *)s : "");
}

void loop() {
  myusb.Task();
  bool now = midi1.idVendor() != 0;   // attached
  if (now != attached) {
    attached = now;
    if (now) printDevice();
    else Serial.println("MIDI device gone");
  }
  while (midi1.read()) {
    uint8_t type = midi1.getType();
    messages++;
    if (type == MIDIDeviceBase::SystemExclusive) {
      uint16_t n = midi1.getSysExArrayLength();
      sysexBytes += n;
      Serial.printf("SysEx %u bytes: %02X %02X %02X ...\n", n, midi1.getSysExArray()[0],
                    n > 1 ? midi1.getSysExArray()[1] : 0, n > 2 ? midi1.getSysExArray()[2] : 0);
    } else if (type != MIDIDeviceBase::Clock && type != MIDIDeviceBase::ActiveSensing) {
      Serial.printf("type %02X ch %2u  %3u %3u\n", type, midi1.getChannel(), midi1.getData1(),
                    midi1.getData2());
    }
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'b') {
      midi1.sendNoteOn(0, 127, 14);
      delay(50);
      midi1.sendNoteOff(0, 0, 14);
      Serial.println("sent BD trig (auto channel 14, note 0)");
    } else if (c == 'c') {
      midi1.sendControlChange(15, 44, 1);
      Serial.println("sent CC 15 = 44 on channel 1 (BD -> EXT)");
    }
  }
}
