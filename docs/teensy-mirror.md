# usb=mirror through a Teensy: a second box shown on a second cable

Current state (2026-11-15, KB analog-four-mkii hardware-tests T335): the real A4 (on the Pi's USB) works through the relay
with the Overbridge Engine on the computer: the link carries the A4's full rate (16,423 IN blocks, none dropped, every
link transfer done under 2 ms) and the Engine's latency check passes (1056-1061 samples, 22.0-22.1 ms). Not yet run: audio
through it with an app (ASIO loop), and a long run. The Pi can also rewrite the Teensy's program (docs/teensy-flash.md).

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

## Pi runs (KB analog-four-mkii hardware-tests T314-T326)

**State (2026-11-14, T326):** check 3 passes on the Pi with relaytest=1: every bench stage PASS (control requests, Pi ->
computer audio with no corrupt or duplicated block, computer -> Pi audio, pipe echo lossless), worst Teensy wait 625 us
against the ~1.2 ms margin. Build: teensy-relay-fixes b39f391, rpiboot folder testbuild-teensyrelay-test3-ramdisk.

- The corrupt Pi -> computer blocks of T323 were the A4's own Overbridge audio (the A4 is on the Pi's USB; rytmob2
  streams it): relaytest's "feeding" flag was set across TestPoll, so the box's blocks (xHCI IRQ) passed FromBoxBlock's
  test-mode gate (T324, bench `--dump`). A flag only around each call still let a few through (T325). Fixed in b39f391:
  TestPoll sends through `SendBoxBlock`; FromBoxBlock drops every real box block in test mode.
- The long Teensy waits (2.7-3.2 ms) were the Python reader: 8 reads of 8 blocks queued. With `--read-blocks 32
  --read-depth 32` the worst is 0.5-0.6 ms (T324-T326).
- Left: 0.6-0.8 % of relaytest's blocks lost. The Pi's test feed runs in core 0's loop and bursts up to 16 blocks when
  the loop was held; the link's 16 URBs and the Teensy's 16-block queue overflow then (Pi drops ~6.5k, Teensy ~7.2k per
  30 s). A real box's blocks come from the xHCI IRQ at a steady rate, so this is a test-mode limit; the A4 itself through
  the relay is the check.
- The bench counts a pipe echo left from the previous stage as stale (it made every echo count as a duplicate, T325).
- The Pi reset with the relay attached until 72e4a61: a no-data link request passed a buffer, and Circle's xHCI asserts
  `nBufLen > 0` on a URB with a buffer at completion (xhciendpoint.cpp(450)). The same case is fixed in ob_usb.cpp's pipe
  OUT and forwarded control paths (a zero-length packet, a request with wLength 0).
- Debug options kept in the driver: `relaydelay=N` (start N s after boot, past the black box's first write at 5 s),
  `relaydebug=1` (each start step and link request written to the black box at once), `relayvariant=` (TR1g's bisect).
  A panic's line reaches the black box (`blackbox_pi::PanicFlush`, ported from 8f027d3).
- relaytest uploads the bench's BOS and MS OS 2.0 set: the made-up box says bcdUSB 2.10 and Windows refused it without.

## The A4 through the relay (T327-T335)

- **T327 (TR2):** "Latency measurement failed": the link carried ~18 % of the A4's IN blocks (the Pi's 16 link URBs busy).
- **T328 (TR3):** relaytest carried the full rate with the Engine's pattern (bench `--paths engine --late 2`: reads start
  late, one OUT block per IN block) and with blocks sent from the box's IN completions (`relayfeed=irq`): neither is it.
- **T329/T330 (TR2b/TR2c):** link counters on both ends. The Pi's link URBs completed 4-8+ ms after submit; the Teensy
  never NAKed the link (ENDPTNAK polls 0), never ran out of armed dTDs, its interrupts ran <= 20 us: the Pi's host
  controller sent the link's interrupt OUT only ~1.2 times per ms.
- **Cause:** the A4's async pipe, interrupt IN 0x82 (64 B, bInterval 1), which MirrorAttach keeps a request on so the
  computer gets its async packets. Polled every microframe beside the link, it left the link one transfer per frame.
  The A4 does not stream at all with it not polled (T333, but that kernel had the bug below).
- **Fix (T335, TR2e):** the relay's box polls its interrupt pipes every 1 ms at most (bInterval 4; `mirrorpipeiv=N`
  sets it, 0 the box's own; the USB-C gadget's box keeps its own). The link then carried the full rate and the
  Engine's latency check passed. The Engine stops streaming by itself after the check while no app uses the A4.
- f56dc30..b408c9b had a bug: MirrorAttach skipped parsing the box's configuration for slots 0/1 (fixed in 29fe713);
  T331-T333 ran with it and do not count.
- Kept for later runs: link 0x02 counters (Pi stats line "relay link 0x02"), the Teensy's link counters ("relay teensy:
  isr max ..."), the relay box's configuration in hex and its pipes repeated in the stats for 2 minutes,
  `relaytest=1 relayfeed=irq`, bench `--paths engine --late N`. `relayboxpipes=1` (relaytest with the real box's
  pipes) does not stream (T332) and is not to be used.

## Gaps (named, not done)

1. **The gadget's own copy** of the audio steps (sidecar_gadget.cpp) could use `mirror_stream.h` too; left alone while the
   two-links branch changes that file.
2. **relaytest's feed** bursts after a held loop (see above); a timer-paced feed would let the test count losses exactly.

## Next

1. Audio through the relay with an app: the 60 s ASIO sine loop of go-TR2 (tr2_loop.py with a timeout), then a long run.
