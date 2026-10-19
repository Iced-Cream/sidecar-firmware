# Measurements in rytm-emu (2026-10-07)

Rytm MKII OS 1.73 with rytm-mods efm-173 (b0a928a, FM4 and the other digital
engines), in rytm-emu, from a fresh cold boot. Headless (`dtboot`).

## SysEx into the Rytm over USB MIDI

Messages `F0 7D <data> F7` (7D: the non-commercial manufacturer ID, which the OS
does not recognise), fed into the USB MIDI input for 2 s with
`dtboot --usb-midi-file F@ATxN/SPACING` (200 M instructions = 1 emulated second):

| message | rate offered | CPU executed / 500 M | idle | MIDI pool in use after |
|---|---|---|---|---|
| none | - | 254.19 M | 49.2% | 0 |
| 1000 B | 100 KB/s | 254.45 M | 49.1% | 0 |
| 4000 B | 400 KB/s | 254.99 M | 49.0% | 0 |
| 4000 B | 800 KB/s | 255.78 M | 48.8% | 0 |
| 4000 B | 1.6 MB/s | 257.37 M | 48.5% | 0 |

[measured] The OS's MIDI task takes every message (all 1024 pool blocks free and
the USB MIDI credit semaphore back at 128 afterwards) and discards unknown SysEx at
about 1 instruction per byte. Rytm-side CPU is not the limit for SysEx transport.

[not modelled] The emulator hands USB MIDI to the OS at the MIDI task's queue
(`Machine::feed_usb_midi`): the USB controller, its interrupts and the endpoint
copies are not emulated, so their cost is missing, and so is the link's bandwidth.

## What audio over SysEx would need

14-bit samples as two 7-bit data bytes: one 48 kHz mono stream is 96 000 data
bytes/s, about 128 000 bytes/s on the wire as USB-MIDI event packets (3 data
bytes per 4-byte packet). Eight streams: 768 KB/s data, about 1.02 MB/s on the
wire. That exceeds what full-speed USB bulk can carry in practice (about 1 MB/s
at best), so 8 voices over SysEx need a high-speed link; 2..4 voices fit full
speed [inferred; see research-rytm-usb.md for the Rytm's link speed].

The Rytm-side handler (a mod hooking the SysEx receive path for 7D) would unpack
into each voice's digital-layer buffer A (32 Q31 samples a block, as FM4 renders),
a few instructions a byte: roughly 2% of the CPU for 8 voices [estimate].
