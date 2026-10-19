# First test with the Teensy 4.1 (prepared 2026-10-07)

Goal: does the Teensy, as USB host, see the Rytm MKII and its MIDI? Nothing here
touches the Rytm's firmware; it works with your stock OS 1.73.

Note: flashing this replaces whatever the Teensy runs now (M8 headless). You can
flash M8 headless back afterwards (its own instructions / the M8 headless page).

## Tools (not installed on this Mac yet)

Arduino IDE 2 with PJRC's Teensy support: Settings > Additional boards manager
URLs: `https://www.pjrc.com/teensy/package_teensy_index.json`, then Boards Manager >
"Teensy" > install. USBHost_t36 comes with it. (Or arduino-cli with the same URL.)

## Wiring

- Teensy micro-USB -> Mac (power and serial monitor).
- The Teensy 4.1's 5-pin USB host header -> a USB-A socket cable (PJRC sells one)
  -> the Rytm's USB port (Rytm USB-B, so an A-to-B cable).
- The Rytm has its own power supply.

## Run

1. Open `teensy/usbhost_probe/usbhost_probe.ino`, board "Teensy 4.1", USB type
   "Serial", upload.
2. Serial monitor at 115200. Try each of the Rytm's USB modes (SETTINGS > SYSTEM >
   USB CONFIG; the Rytm may need a restart): USB MIDI, USB AUDIO/MIDI, OVERBRIDGE.
   For each, note whether "MIDI device attached: VID 1935 PID ...." appears and
   what it says.
3. With MIDI out to USB enabled on the Rytm (GLOBAL > MIDI CONFIG > PORT CONFIG >
   OUTPUT TO includes USB), hit pads and turn knobs: messages should print.
4. Type `b` + Enter: the Rytm's BD should play (the Teensy sending MIDI to it).

What to bring back: the attach line for each USB mode, and whether 3 and 4 work.
Expected PIDs (research-rytm-usb.md): Overbridge 0x0B48, USB MIDI 0x0011, USB
Audio/MIDI 0x1048, vendor 0x1935 [from public sources, unverified].
