# USB audio on the Sidecar Mini (Teensy 4.1)

The Sidecar Mini is the Rytm's USB host (Overbridge on the Teensy's USB host port). On its own micro-USB port the
computer sees one composite USB device, **"Sidecar Mini"**: the serial console (the web console finds it by PJRC's
vendor ID 0x16C0, unchanged) and a class-compliant **USB Audio Class 2** interface. No driver on macOS, Linux, iPadOS
or Windows 10 1703+.

## What the Mini offers (`teensy/host_stereo.sh`, the release build)

| | |
|---|---|
| Audio | 2 in / 2 out, 48 kHz, 24-bit |
| IN 1/2 (to the computer) | the Rytm's MAIN L/R as its Overbridge sends it: the Rytm's whole mix, with the Sidecar's voices as the Rytm plays them (with `out=tracks` each voice goes through its track's filter, FX and level) |
| OUT 1/2 (from the computer) | added to the Rytm's MAIN L/R Overbridge input (`:mac=off` drops it) |
| Clock | the Rytm is the master: the IN packet sizes follow the Overbridge frames (11/12/13 a packet), the OUT feedback is the Rytm's rate against the computer's USB clock (explicit feedback, async) |
| USB MIDI | the Rytm's MIDI to the computer and the computer's to the Rytm (usb_host.h: everything but our host link's SysEx; to the Rytm channel messages, clock, transport and whole SysEx but Elektron's), as the Pi; small buffers (2 x 512 B receive, 4 x 128 B transmit: short packets) |
| While recording to the card | without PSRAM the recorder's 32 KB ring is shared with USB audio's rings: while it is armed or records, USB audio is **paused** - the device stays (no re-enumeration), IN is silence, OUT is dropped; back when the take ends. USB MIDI runs throughout. With PSRAM nothing is paused. Shown on the USB tab ("paused while the recorder..."), the Record tab and the Rytm's REC page ("USB OFF") |

`:usbmode=stereo` (the default) or `:usbmode=off` (Serial only, no audio device), also on the web console's USB tab.
Kept across reboots. A change re-enumerates the Teensy: the serial port is gone for about 2 s and the web page
reconnects by itself; the Rytm keeps streaming. Audio apps (Ableton etc.) need the device re-selected after a change.

The web console's USB tab also shows whether an app on the computer has the audio interface open (recording,
playing, or idle).

macOS may make "Sidecar Mini" its default output when it appears: system sounds then go to the Rytm's MAIN input.
Pick another output in Sound settings, or `:mac=off`.

## Measured (2026-10-18, MacBook, macOS 26.7; the Rytm streaming on the Teensy)

- CoreAudio: "Sidecar Mini", 2 in / 2 out, 48000 Hz.
- 180 s full duplex (PortAudio float32, 480-frame blocks, OUT silence): 8.64 M frames, PortAudio status flags 0,
  IN 1/2 at -8.7 / -8.6 dBFS rms (the Rytm playing), 0 runs of all-zero frames.
- Teensy: IN 8000 packets/s, underruns 0, overruns 0; OUT ring 29..68 frames (target 48), under 0 over 0, bad 0;
  feedback 47997..48001 Hz, base 47999.1..48001.3 Hz; 8000 SOF/s; Overbridge 47999 Hz, xruns 0, FIFO min 75.
- The console answered during the stream (`:sendfx`, `:rec`, `:usbmode`, `:mac`). Plugin store: 20 installed.
- The user heard the Rytm on the Mini's stereo input in Ableton.

## Memory (the host build's limits)

Before the 1018 diet (mini-diet): ITCM 4 blocks, headroom 4504 B; RAM1 locals 34,816 B (the run_sketch.sh floor);
RAM2 heap 3,392 B (the plain host build: 6,400), no USB MIDI, a 16 KB recorder ring. After it (and with USB MIDI, the
32 KB shared ring): ITCM headroom 10,776 B, RAM1 locals 41,600 B, RAM2 heap 8,704 B (the plain host build: 11,816 B,
60,096 B, 6,400 B). What moved: every string literal and SdFat's / littlefs's tables to flash, USB host enumeration
and boot-only code to flash (cold_ld.sh), the host build's render ring 1024 -> 256 frames, Serial's transmit buffers
4 x 1 KB, the UAC2 DMA buffers into RAM1 and its rings into the shared pool (rec_mini.h).

## The 20 in / 12 out build (`teensy/host_uac2.sh`, testing)

Serial + USB MIDI + UAC2 20 in / 12 out (IN 1-12 the Rytm's Overbridge channels, 13-20 the Teensy's voices; OUT 1-12
the Rytm's Overbridge inputs), `:usbmode=multi|stereo|off`. Since the recorder's 16 KB ring is in RAM1 it no longer
fits (RAM1 over by about 5 KB): needs PSRAM or less RAM1 elsewhere. Details:
docs/superpowers/specs/2026-10-16-overbridge-passthrough-spike.md.
