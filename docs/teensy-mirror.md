# usb=mirror through a Teensy: a second box shown on a second cable

Current state (2026-11-14): the relay runs end to end on a Pi 4 (TR1 tries 5-6, `relaytest=1`: a made-up box, no Elektron
unit): the box enumerates on the computer through the Teensy, forwarded control requests, pipes and the computer's audio
to the Pi pass. Open: the Pi's audio to the computer had 4.5 % of blocks with wrong content and 2 % lost, with the
Teensy's queue full (worst wait 2.7 ms against the ~1.2 ms margin), read by a Python bench; not yet run with a real box.

The Pi's USB-C port is one USB device, so `usb=mirror` (docs/usb-mirror.md) shows one box. A Teensy 4.1 adds a second
device port: the Pi stays the host of both boxes and keeps all of the mirror's logic, and the Teensy only relays.

```
Rytm --USB--> Pi USB-A ... Pi USB-C (DWC2 gadget, usb=mirror) ----------------------> computer 1
A4   --USB--> Pi USB-A (host, its Overbridge stream as today)
              Pi USB-A (host) --USB--> Teensy USB1 (micro-USB: the link, 1209:0002)
                                       Teensy USB2 (5-pin header: the A4 as the computer sees it) --> computer 2
```

`mirror=rytm|a4` on the cmdline picks which box the USB-C gadget mirrors; the relay mirrors the other one. Both need the
same opt-in as today's mirror (the USB-C mode OVERBRIDGE or `usb=mirror`): a relay alone never shows a box maker's IDs.

## Pieces

| piece | where | what |
|---|---|---|
| link protocol | `pi/engine/relay_proto.h` | fixed link layout, vendor requests, events, channel map, control slot; `tests/test_relay_proto.cpp` |
| audio for a second port | `pi/engine/mirror_stream.h` | the gadget's mirror audio steps (alternate -> channels, IN cut, OUT ring, per-block bytes) for any port; `tests/test_mirror_stream.cpp` |
| Pi target | `pi/engine/teensy_target.{h,cpp}` | claims 1209:0002 (`circle-usbdevicefactory3.patch`), registers as the other slot's `IMirrorTarget` |
| Teensy firmware | `teensy/mirror_relay/` | own ChipIdea device driver (`ci_dev.*`) on both ports; the relay (`relay.cpp`); `build.ps1` |
| bench | `teensy/mirror_relay/bench/relay_bench.py` | the PC plays the Pi on USB1 and the computer on USB2, with a made-up device |

## The link (relay_proto.h)

- USB1, high speed, one vendor interface: events on interrupt IN 0x81; the computer's audio OUT blocks on interrupt IN
  0x82; audio IN blocks for the computer on interrupt OUT 0x02; four bulk channel pairs (0x83..0x86 / 0x03..0x06) for the
  other endpoints (pipes, MIDI). One mirrored packet per link packet.
- Both sides derive the channel map from the box's configuration descriptor (`mirror::parse`), so nothing about it is sent.
- EP0 on the Teensy: standard requests answered there (descriptors as uploaded, SET_ADDRESS, SET_CONFIGURATION,
  SET_INTERFACE kept local as the gadget keeps it, GET_STATUS, endpoint halt); every class and vendor request goes to the
  Pi as an event and EP0 NAKs until the Pi answers (`kReqCtlAnswer`). A new SETUP ends the one in flight (`kEvAbort`).
  The name suffix and the filters stay on the Pi.
- Flow: a pipe OUT packet from the computer is taken only with a credit from the Pi (one per packet the box took), as the
  gadget NAKs until the box took it; a pipe IN packet's next box request waits for `kEvInTaken`. Audio has no flow control;
  each side keeps a short queue and drops the oldest block when full (counted).
- `kReqBootloader` (wValue 0x4C42) reboots the Teensy into its bootloader, so a reflash needs no button press.

## Bench results (2026-11-14, Teensy alone on the PC, no Pi, no box)

- The link enumerates at high speed (WinUSB from MS OS 2.0), the made-up box on USB2 at high speed.
- Audio both ways plus both pipes together for 20 s: no duplicated, reordered or corrupt block, pipes lossless both ways
  (5999 packets each), every pipe IN packet answered by `kEvInTaken`. Pi->computer ran at the box's rate (6,845 blocks/s
  of 6,857, 0.14 % dropped by the Teensy's 16-block queue when the Windows reader lagged at URB boundaries). The
  computer->Pi stream ran at about 4,400 blocks/s, not 6,857: the Python sender is the limit; on the Teensy its blocks
  waited under 250 us (worst 267 us).
- Fixed on the way: the controller's interrupt threshold (ITC) left at its reset value of 8 microframes held completions
  back to one batch per millisecond (2000 blocks/s); `ci_init` now sets it to 0.
- Forwarded control requests (staged run, 15 s a stage, then all paths at once): 650 vendor IN/OUT requests of 16-127
  bytes, answered by the "Pi" through the link, all correct; round trip median 6.8 ms (8.6 ms with every stream running),
  the Python "Pi" being most of it. One request a stage took about 1 s (not yet explained; to watch on the Pi).
  With the link behind one of the PC's Genesys USB 2.0 hubs and the box's port behind another, Windows held every control
  transfer to the link while the box's request was NAKing, so nothing could be answered; with the link on a root port
  of the same controller it works. On the real setup the link is on the Pi.

## USB MIDI

The box's MIDI function is relayed like a pipe pair. On the Pi, `link1::SetTap` gives the target every event packet the
slot-1 box sends (besides the engine), batched to the Teensy; the computer's packets go through a `MidiFilter` (OS updates
to the box only with `usbmidi=os:on`) into `link1::FromComputer`, sent from link1's `Send` ahead of the engine's records,
which wait while the computer's SysEx is part way out. Only the slot-1 box (`mirror=rytm`, the default): with
`mirror=a4` the relay shows the Rytm, whose MIDI is the host link, and its MIDI is not relayed.

## Pi runs (KB analog-four-mkii hardware-tests T314-T323)

- The Pi reset with the relay attached until 72e4a61: a no-data link request passed a buffer, and Circle's xHCI asserts
  `nBufLen > 0` on a URB with a buffer at completion (xhciendpoint.cpp(450)). The same case is fixed in ob_usb.cpp's pipe
  OUT and forwarded control paths (a zero-length packet, a request with wLength 0).
- Debug options kept in the driver: `relaydelay=N` (start N s after boot, past the black box's first write at 5 s),
  `relaydebug=1` (each start step and link request written to the black box at once), `relayvariant=` (TR1g's bisect).
  A panic's line reaches the black box (`blackbox_pi::PanicFlush`, ported from 8f027d3).
- relaytest uploads the bench's BOS and MS OS 2.0 set: the made-up box says bcdUSB 2.10 and Windows refused it without.

## Gaps (named, not done)

1. **Pi build and run:** the kernel with `teensy_target.o` and `circle-usbdevicefactory3.patch` is not built yet;
   then check 3 of two-boxes.md (the Pi forwards a box to the relay; jitter against the 8-block margin).
2. **The gadget's own copy** of the audio steps (sidecar_gadget.cpp) could use `mirror_stream.h` too; left alone while the
   two-links branch changes that file.

## Next (handed over 2026-11-14)

1. Pi -> computer audio: dump the bad blocks on the bench (other blocks' data, shifted, or half-written?); suspects are the
   Pi's 16-buffer pool in `FromBoxBlock`, the Teensy's `aq_pc` ring when it drops the oldest, and the reused
   `m_pAudioOutURB[k]`.
2. The same burst pattern (up to 16 blocks back to back, as relaytest's feed) from a PC playing the Pi, to see whether the
   Teensy alone corrupts.
3. A faster reader than the Python bench, to tell its limit from the relay's.
4. Then a real A4 on the Pi's other USB-A port with Overbridge on the computer.
