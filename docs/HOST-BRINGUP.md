# The Teensy as the Rytm's USB host: bring-up (prepared 2026-10-09)

Goal: the Teensy 4.1 as the USB host of the real Analog Rytm MKII in OVERBRIDGE
mode, replacing today's chain (Mac: rytm-emu's simulated host, its `--ob-link`
TCP, `sim/teensy_bridge.py` over serial). Written before the host cable arrived:
everything here compiles, the wire format is checked against the emulated Rytm,
and nothing has run against a real Rytm yet. Tags as elsewhere: [measured]
(in rytm-emu with the Rytm's own OS 1.73, unless it says hardware), [inferred],
[untested].

## What there is

| file | what |
|---|---|
| `teensy/libraries/RytmHost/ob_block.h` | Overbridge 2 block format, voice/channel tables, descriptor scan; no Arduino dependency |
| `teensy/libraries/RytmHost/RytmHost.{h,cpp}` | `RytmOverbridge`, a USBHost_t36 driver: claims interface 1, SET_INTERFACE 1/2 alt 3, CLEAR_FEATURE(HALT) 0x83/0x03, then the audio both ways |
| `teensy/libraries/RytmHost/extras/ob_block_test.cpp` | host test of ob_block.h against the Rytm's configuration descriptor |
| `teensy/obhost_bringup/obhost_bringup.ino` | the step-by-step sketch below (descriptors, MIDI, IN only, both ways, test tone) |
| `teensy/voiceloop/usb_host.h` + `RYTM_USB_HOST` in voiceloop.ino | the voice loop on the Rytm directly |
| `teensy/usbhost_probe/usbhost_probe.ino` | the first probe (stock `MIDIDevice_BigBuffer` only), unchanged |

USB MIDI is USBHost_t36's own `MIDIDevice_BigBuffer`: the Rytm's MIDI
interface (5) is plain class 1/3 with 512-byte bulk endpoints, which it claims
by itself; it rejects the Rytm's vendor interfaces 0-3 and the audio control
interface 4 (checked against `midi.cpp` claim(): no MIDI jacks or no endpoints)
[inferred from the source]. Nothing in USBHost_t36 is patched.

### What USBHost_t36 lacks, and what RytmHost does about it

- No Overbridge or audio class at all. Overbridge 2 needs no isochronous: high
  speed interrupt pipes do, which the library has (`new_Pipe` type 3, bInterval
  1 = every 125 us microframe, high-bandwidth mult fixed at 1 = one packet of
  up to 1024 bytes a microframe; 368 fits).
- Errors on interrupt pipes are never reported (`followup_Error()` walks only
  the async list): a halted qTD just stops the stream. USBHost_t36 leaves CERR
  at 0, so bus errors retry forever and a halt means a STALL, babble or a
  buffer error. `RytmOverbridge::Task()` checks once a second: no IN for 50 ms
  is logged with both queue heads' tokens; a halted pipe gets
  CLEAR_FEATURE(ENDPOINT_HALT), and its completion re-arms the halted qTD in
  place (`rearm()`) [untested]. IN transfers lost to a full Transfer_t pool are
  queued again (`queue fails` in the log).
- A control request that fails (STALL) never calls the driver back. Each setup
  step times out after 300 ms with a log line and the sequence goes on; each
  step has its own setup packet and only its own completion advances, so a
  late answer cannot skip one.

### Design

- **Clock**: the Rytm is the master. It sends 48000 / 7 = 6857 blocks a second
  on 0x83; the host polls every microframe (8000 a second), the Rytm NAKs the
  rest. For each block received, one block is sent on 0x03 (credit, as rytm-emu's
  host and Overwitch pace it) - so the OUT rate is the IN rate by
  construction, no drift handling needed.
- **Transfers**: 4 IN and up to 4 OUT transfers in flight, 2 blocks (14 frames,
  0.29 ms) each (`RYTM_OB_XFERS`, `RYTM_OB_BLOCKS_PER_XFER`): about 3400
  completions a second each way, in the USB host interrupt.
- **Render on demand**: the OUT blocks take 7 frames each from a FIFO (512
  frames, `RYTM_OB_FIFO`), filled by `loop()`: `host_feed()` renders voiceloop's
  12-frame blocks until the FIFO holds `host_target` frames (256, 5.3 ms,
  `RYTM_HOST_TARGET`; `+`/`-` over serial). A block the FIFO cannot fill goes out
  with silence and counts an xrun (once the FIFO has held frames).
  Card refills and saves run only while the FIFO is at its target (as they ran
  between serial requests before). 256 frames covers the longest stalls measured
  in PLAN 3c (a card read <= 0.8 ms, a project change 3.8 ms) [inferred].
- **Latency** (host -> Rytm) [inferred]: FIFO target + OUT in flight (up to 56
  frames) + the Rytm's own buffer. 256 -> about 6.5 ms plus the Rytm's; today's
  bridge keeps 512 frames (10.7 ms) plus the Mac's. A trig reaches the Teensy
  over USB MIDI in well under a millisecond and sounds after the FIFO.
- **Channels**: voices go to their own inputs, `ob::kVoiceToRytm` = {4, 7, 5, 8,
  10, 9, 11, 6} for physical voices 0..7 (the firmware's table,
  docs/overbridge-voice-inputs.md; the bridge's `VOICE_CH`), int16 << 16. All
  other host -> Rytm channels are zero. The Rytm's 12 channels in are parsed,
  their peaks kept for the log, and available through `setRxCallback`
  (voiceloop does not use them yet: DAISYSP's INP from another voice could).
- **voiceloop with `RYTM_USB_HOST 1`**: the Rytm's F0 7D 52 54 reports go to
  `report()`, as the bridge's `S` frames did; `to_rytm()` (14 15 16 17, 0E, ...)
  sends SysEx over USB MIDI instead of `X` frames; clock and transport are
  counted and logged. The log is plain text lines then (no `L` framing), so any
  serial monitor reads it; `teensy_bridge.py` is not used. With the switch off
  (the default) the sketch builds as before (the same sizes, below).

### The Rytm's descriptors (OVERBRIDGE mode) [measured in rytm-emu, OS 1.73]

Device 1935:0B48, bcdUSB 2.00, class EF/02/01 (interface associations), EP0 64
bytes, self-powered. Configuration 1, 263 bytes, 6 interfaces:

| interface | alt | class | endpoints |
|---|---|---|---|
| 0 | 0 | FF vendor | - |
| 1 | 0 / 1 / 2 / 3 | FF | 0x83 interrupt IN: - / 32 / 88 / 368 B, bInterval 1 (alt 1 a bare header, alt 2 2 channels, alt 3 12) |
| 2 | 0 / 1 / 2 / 3 | FF | 0x03 interrupt OUT: the same sizes |
| 3 | 0 | FF | 0x02 bulk OUT 512 B, 0x82 interrupt IN 64 B bInterval 1 (not used here) |
| 4 | 0 | 01/01 audio control | - |
| 5 | 0 | 01/03 MIDI streaming | 0x01 bulk OUT 512 B, 0x81 bulk IN 512 B; one embedded and one external jack each way |

(Alt 1's 32 bytes would be a header with no samples; `ob::channels_for` says 0
channels, so the driver would not take it.) The full bytes are in
`extras/ob_block_test.cpp`. The same layout is what Overwitch expects for the
Rytm MKII (pid 0x0B48, interfaces 1/2 alt 3, 12 + 12 tracks).

## Build

From `teensy/` (the IDE's arduino-cli; Teensyduino 1.62):

    CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
    "$CLI" compile --fqbn teensy:avr:teensy41:opt=o2std --libraries ./libraries ./obhost_bringup
    "$CLI" compile --fqbn teensy:avr:teensy41:opt=o2std --libraries ./libraries ./voiceloop      # serial bridge (default)
    "$CLI" compile --fqbn teensy:avr:teensy41:opt=o2std --libraries ./libraries ./voiceloop \
        --build-property "build.flags.defs=-D__IMXRT1062__ -DTEENSYDUINO=160 -DRYTM_USB_HOST=1"   # USB host

(or set `#define RYTM_USB_HOST 1` at the top of voiceloop.ino in the IDE). Upload
as usual (`teensy/run_sketch.sh obhost_bringup` compiles, uploads and shows the
serial output; it does not pass the define). The host test, on the Mac:

    c++ -std=c++17 -Wall -I teensy/libraries/RytmHost teensy/libraries/RytmHost/extras/ob_block_test.cpp -o /tmp/ob_test && /tmp/ob_test

Memory (compile lines, 2026-10-10, Teensyduino 1.62, linked with `teensy/cold_ld.sh`: LittleFS's core
and SdFat in program flash; `loop`, `report`, `poll_input`, `host_poll` in FLASHMEM; plugins on):

| sketch | ITCM code | RAM1 free (stack) | RAM2 free |
|---|---|---|---|
| voiceloop, `RYTM_USB_HOST 0` | 143.5 KB (5 blocks) | 78.7 KB | 37.7 KB |
| voiceloop, `RYTM_USB_HOST 1` | 157.8 KB (5 blocks, 6.1 KB spare) | 45.3 KB | 37.7 KB |
| voiceloop, `RYTM_USB_HOST 0`, `RYTM_PLUGINS 0` | 124.9 KB (4 blocks) | 154.5 KB | 41.2 KB |
| voiceloop, `RYTM_USB_HOST 1`, `RYTM_PLUGINS 0` | 137.5 KB (5 blocks) | 93.2 KB | 41.2 KB |
| obhost_bringup (2026-10-09) | 57.3 KB | 418.7 KB | 511.9 KB |
| usbhost_probe (2026-10-09) | 46.8 KB | 441.0 KB | 511.9 KB |

Without those two changes the host build with plugins needed 7 ITCM blocks and had -19.9 KB for
locals (it did not link). The host build costs ~14 KB of ITCM (USBHost_t36, MIDI, the driver's data
path; its setup and logging run from flash) and 33 KB of RAM1 (the FIFO 8 KB, transfer buffers 6 KB,
USBHost_t36's periodic table and MIDI buffers; RytmHost.h: the instance stays in DTCM, uncached, so
its DMA needs no cache maintenance). A hub between Teensy and Rytm needs `-DRYTM_HOST_HUB=1` (1.5 KB
more ITCM). The serial port of the host build: plain log lines and the monitor keys, until a
development tool sends a framed command (`tools/rmp_install.py --host`, `sim/rmp_e2e.py --host`);
then the serial build's framing until a reboot, `'X'` copies of what goes to the Rytm included. The tool
sends `'P'` and skips everything up to the framed `pong`. Without a Rytm streaming Overbridge audio, `'N'`
renders the voices as on the serial build, so `sim/rmp_e2e.py --host`'s sine checks need no Rytm; while
the Rytm streams, the voices go to it and `'N'` is answered with silence.

## Monday: step by step

Wiring: Teensy micro-USB -> Mac (power, serial); Teensy 5-pin host header ->
USB-A socket cable -> A-to-B cable -> the Rytm's USB. The Rytm on its own
supply. No hub at first. Serial: the Arduino IDE's serial monitor (it sends
what you type; one character per command, then Enter), or
`screen /dev/cu.usbmodem* 115200` (keys go straight through).

On the Rytm: SETTINGS > SYSTEM > USB CONFIG = OVERBRIDGE (restart if asked);
SETTINGS > MIDI CONFIG > PORT CONFIG: OUTPUT TO includes USB, and for clock
SYNC: CLOCK SEND / TRANSPORT SEND on. The stock OS is enough for steps 1-5; the
patched firmware (rytm-mods 0021 EXT ...) for step 6.

### 1. Enumerate (usbhost_probe, unchanged)

Flash `usbhost_probe`. Expect `MIDI device attached: VID 1935 PID 0B48
manufacturer 'Elektron...' product 'Analog Rytm MKII...'` in OVERBRIDGE mode
(0011 in USB MIDI, 1048 in USB AUDIO/MIDI). `b` plays BD.

- Nothing at all: the cable/header (5 V on the host header? the Rytm's USB LED
  / its screen showing a connection), try another A-to-B cable.
- Attached in USB MIDI mode but not OVERBRIDGE: go on with step 2 anyway (it
  prints why), and note it.

### 2. Descriptors (obhost_bringup)

Flash `obhost_bringup`. On plugging in: `Elektron device 1935:0B48 enumerated at
480 Mbit (high speed)`, then `ob: Rytm 1935:0b48, 12 ch in / 12 ch out, packets
368/368 B, bInterval 1/1 - waiting for start()`. Type `d`: the parsed list and
hex. Compare with the table above; record any difference (that is the most
valuable output of the day if it differs).

- `12 or 1.5 Mbit`: the link came up full speed - the cable or the header
  wiring (D+/D- swapped or long leads), or a full-speed hub. Overbridge 2 will
  not run; MIDI may.
- `Overbridge driver: refused - no interrupt endpoints ...`: not in OVERBRIDGE
  mode, or a different layout: send the `d` output.

### 3. MIDI only (still obhost_bringup, no `g`)

- `MIDI attached: 1935:0B48 ...`.
- Hit pads / turn knobs: `MIDI 90 ch 10 ...` lines (note / CC per the Rytm's
  MIDI settings).
- `b`: BD plays (the Teensy sending a note over USB MIDI).
- Press PLAY: `transport START`; the stats line needs `g`, so check the clock
  with step 4's lines (`midi: N clocks (BPM)`) or in step 6's log.
- Patched firmware with an EXT track: a trig prints `SysEx 41 B: F0 7D 52 54 04
  ...`.

Fallback: if MIDIDevice does not attach in OVERBRIDGE mode, try USB MIDI mode
(the driver then refuses, MIDI should work): that isolates the Overbridge
interfaces from MIDI.

### 4. Audio Rytm -> Teensy (`o`, then `g`)

`o` (OUT off: nothing sent to the Rytm), then `g`. Expect `ob: SET_INTERFACE 1
alt 3` ... `ob: streaming` and once a second:

    ob streaming: in 48000 Hz (6857 blocks), out 0 Hz; fifo 128 ...; xruns 0 ...; bad 0, counter gaps 0, ...
      peaks (x1000): main 0 0 | BD SD RS BT LT MT CH CY 0 0 ... | in 0 0
      midi: 0 clocks (0 BPM), 0 SysEx in

- `in` should be 48000 +- a few (the Rytm's clock against the Teensy's); blocks
  6857 a second. Play pads: MAIN and the voice's column rise (BD is the first
  voice column). Signal into AUDIO IN L/R: the `in` columns.
- `h`: the first IN block's 32-byte header: `07 00 <counter>` and the 28 unknown
  bytes - write them down (open question: what the Rytm puts there).
- `bad` or `counter gaps` rising: blocks lost or malformed - note the rates.
- No IN at all (`ob: no IN block for N ms (IN qh token ...)`): decode the token
  - 0x80 active (still waiting: the Rytm NAKs - it is not streaming; try `o`
  again so OUT runs, step 5, in case the Rytm needs both directions), 0x40
  halted, 0x20 buffer error, 0x10 babble (packet longer than 368), 0x08
  transaction error, 0x01 ping state. The driver clears and re-arms a halted
  pipe and says so (`CLEAR_FEATURE sent`, then `rearms` rises).
- A setup step timing out (`got no answer in 300 ms`): the Rytm stalled
  SET_INTERFACE / CLEAR_FEATURE - note which.

### 5. Audio both ways (`o` back on, `t`)

`o` again (OUT on): `out` should now equal `in`, the fifo stay near 128, xruns 0.
`t`: a 440 Hz tone at -12 dBFS on voice 0's input (channel 4, BD) and on MAIN
L/R (`m` toggles MAIN): it should be heard from the main outs at once (MAIN
input needs no trig).

- `m` off: the tone only on BD's input - audible while BD's VCA is open (hold a
  BD trig with a long decay, or the patched EXT machine on BD) [inferred, the
  Overbridge manual].
- `v` steps the voice 0..7 (BD BT SD LT CH/OH MT/HT CY/CB RS/CP - physical voice
  order); each should come out of its own track.
- Wrong voice for a track: the channel table (`ob::kVoiceToRytm`) is wrong on
  hardware - note which voice plays where.
- Clicks with xruns 0: look at `counter gaps` / `bad`; with xruns > 0 the
  sketch did not keep up (it should not, it only copies a table).

Fallback: if OUT breaks IN (in stops when OUT starts), leave `o` off and report
the token lines; the Rytm may need another pacing (e.g. bigger OUT transfers:
`-DRYTM_OB_BLOCKS_PER_XFER=4` or 24 as Overwitch's default).

### 6. The voice loop (voiceloop with `RYTM_USB_HOST 1`)

Flash voiceloop built with the define. The boot log (card, index, pool) as
before, then `usb host: waiting for the Rytm ...`, `usb host: MIDI attached,
1935:0b48`, `ob: Rytm 1935:0b48, 12 ch in / 12 ch out ... - starting`, `ob:
streaming`. Every 2 s, after the two cpu lines:

    usb host: ob streaming, in 48000 Hz out 48000 Hz, fifo 256 (min 240 max 268 target 256), xruns 0 blocks 0 frames,
    bad 0 gaps 0 unanswered 0 rearms 0/0; midi on, clock 120 BPM, sysex in 12 out 0 dropped 0

- EXT tracks: trigs print `trig voice N ...` and sound through the voice's
  analog chain, as in the emulator. Browse TEENSY in the Rytm's sample browser:
  `0D` reports answered with `0E` listings over USB MIDI (`sysex out` rises).
- Watch `fifo min` and `xruns` while stressing (8 voices, sample changes,
  browsing, project changes - PLAN 3c's scenarios): xruns must stay 0. Then
  lower the target with `-` (256 -> 128 -> 64) for less latency until xruns
  appear, and go one step back; `+` raises it. Record the lowest clean target.
- `s` prints the host line at once; `R` reboots.

Fallback: flash voiceloop without the define and run the Mac chain (rytm-emu
`--usb-host --ob-link` + `sim/teensy_bridge.py`) as before - nothing in that
path changed.

## CPU estimate without a Rytm (teensy/obhost_bench, 2026-10-12)

`teensy/run_sketch.sh obhost_bench 50` (8 voices of Plaits MODAL by default;
`RMP_EXTRA_FLAGS="-DBENCH_VOICES=6 -DBENCH_ENGINE=8"` for others). The host port
stays off (`usb.begin()` is never called); nothing needs to be plugged in.

What runs is the real code: RytmOverbridge's `in_done` / `pump_out` /
`fill_out` / `out_done` (unchanged) and USBHost_t36's `queue_Data_Transfer`,
`queue_Transfer`, `followup_Transfer`, `free_Transfer`, in a handler on
IRQ_USBHS at its default priority. Simulated: the EHCI controller (a PIT above
USBHS marks the oldest active qTD done, moves the queue head on, pends
IRQ_USBHS), `USBHost::isr()` (the USBSTS read/clear and its periodic-followup
loop, repeated line for line on the library's own list heads), and a
bus-less high-speed device with the Rytm's two endpoints. Rate: the Rytm's,
48000 / 7 = 6857 blocks a second each way, 2 blocks a transfer: 3429 IN and
3429 OUT completions a second, in separate interrupts (6857/s, the worst
case) or together.

Measured on the Teensy 4.1 at 600 MHz (ARM_DWT_CYCCNT; headroom = busy units
loop() still does, against a calibration with nothing else running):

| | cycles | CPU |
|---|---|---|
| IN completion (2 blocks: BE int32 to native + peaks, IN requeue, OUT block built and queued) | 5270 avg, 5445 max (9.1 us) | 3.0 % |
| OUT completion | 221 | 0.13 % |
| per block, either way | about 1370 | |
| `fill_out` alone (one OUT transfer) | 2417 | |
| host path, total CPU lost by loop() (entry/exit included; the simulation's own PIT 0.1-0.4 %) | | 3.6-3.9 %, 3.5 % less the PIT (3.4-3.7 % with IN and OUT in one interrupt) |
| 8 x Plaits MODAL alone (12-frame blocks, a timed 48 kHz consumer) | | 84.7 % |
| 8 x MODAL + host path, FIFO kept at 256 frames | | 88.0 %: 12.0 % headroom, 0 xruns |
| 7 x MODAL + host path | | 77.3 %: 22.7 % headroom |

So the host path costs about 3.5 % of the CPU, of which nearly all is the
per-sample work in `in_done` (byte-wise BE conversion and the peak meter of 12
channels, about 2500 cycles a transfer - voiceloop installs no rx callback, so
it uses neither) and `fill_out` (about 2400); USBHost_t36's own queue and
followup work is a few hundred cycles a completion. The longest handler, 9 us,
is the longest the host path holds off loop(); the host build has no audio
interrupt to jitter (loop() renders into a 256-frame, 5.3 ms FIFO), so this
only matters against the FIFO, not at all at this size. This replaces the
guess in assumption 8: 8 x MODAL fits with 12 % to spare (voiceloop's own mixing
and card reads come on top: PLAN's 8 x MODAL is 86 % there, against 84.7 % here).

Not measured (estimated): the controller's DMA into DTCM and bus contention,
NAKs, retries and errors, the USB MIDI interface's bulk completions on the
async list (MIDI clock: about 50 a second at 120 BPM), and `Task()` (from
loop(); its health check once a second). The real `isr()` also tests the other
USBSTS bits - a few cycles. These should add well under 1 % [inferred]; the
first hardware test checks it (`max loop` and xruns). Three runs agreed within
0.3 %.

## Assumptions not verifiable without the cable

1. The real Rytm's descriptors equal the emulator's (they come from the same
   firmware image, so likely) [measured in emulator only].
2. The real Rytm streams IN after SET_INTERFACE alt 3 + CLEAR_FEATURE without
   any vendor request first (Overwitch also reads the device name with vendor
   requests 1/2; rytm-emu's host does not, and the emulated firmware streams)
   [measured in emulator only]. If it does not, add the name read (IN, vendor,
   device, bRequest 1, 32 bytes) before the stream.
3. Credit pacing (one OUT block per IN block) is what the Rytm wants on
   hardware (rytm-emu's host does it; Overwitch resubmits on completion and lets
   the device NAK) [inferred].
4. Interrupt IN qTDs of 2 x 368 bytes complete after two full packets; the
   Rytm never sends short packets (a short one would end the qTD early: counted
   `bad`, the next transfer goes on) [inferred].
5. USBHost_t36's interrupt pipe toggles (DATA0 after SET_INTERFACE / CLEAR_FEATURE)
   match the Rytm's [inferred, USB 2.0 spec].
6. The voice -> channel table and the voice inputs entering before the analog
   chain [measured in emulator, disassembly].
7. The Rytm's MIDI interface works alongside the Overbridge stream (it is in
   the same configuration) [inferred].
8. 256 frames of FIFO absorbs the loop's longest stalls [inferred from PLAN 3c,
   measured over serial]; the host interrupt's cost (about 3400 + 3400
   completions a second, a few microseconds each) fits the 13 % left at 8 x
   PLAITS MODAL [inferred - the cpu line does not count interrupt time; check
   `max loop` and xruns].
9. The DAISYSP INP voice routing and the 28 header bytes are not used.

## Open questions for Monday

- The 28 private header bytes, device -> host (`h`).
- The Rytm's IN rate against the Teensy's clock (`in ... Hz`), and its jitter.
- Does OUT need to start with IN (step 4 with `o` off works?).
- Real level: the voice input at -12 dBFS against a stock voice (PLAN item 2).
- Lowest clean FIFO target in voiceloop, i.e. the end-to-end trig latency.

## Checked against the real Rytm from the Mac (2026-10-09)

The user's Analog Rytm MKII, stock OS 1.73, OVERBRIDGE mode, via a USB 3 hub;
the Mac as host with teensy/libraries/RytmHost/extras/mac_obhost.cc (libusb, the
driver's sequence and ob_block.h):
- descriptors: device 1935:0B48, high speed; the 263-byte configuration
  descriptor byte-identical to the emulator's (assumption 1 holds).
- start: claim 1 and 2, SET_INTERFACE alt 3, CLEAR_FEATURE(HALT) 0x83/0x03 -
  streaming at once, no vendor name request (assumption 2).
- 20 s: 137144 blocks in, as many out (one block back per block received),
  48000.4 Hz, 0 counter gaps, 0 short packets, 0 errors (assumptions 3-5, for a
  20 s run); the device header: 07 00, the frame counter (+7), 28 zero bytes.
- a 220 Hz sine at -20 dBFS into physical voice 0's channel (4): heard on BD
  (the user); BD's return channel (2) carried 220 Hz at -20.5 dB, no 330 Hz /
  1 kHz, the other voices at the noise floor (assumption 6 for BD).
- all eight voices (8 tones at once, 200..1000 Hz by physical voice, -26 dBFS):
  each tone back on its own voice's return channel only (BD 200, SD 315, RS/CP
  1000, BT 250, LT 400, MT/HT 630, CH/OH 500, CY/CB 800 Hz), other voices >= 66
  dB down - the table kVoiceToRytm / kVoiceFromRytm holds (assumption 6). A
  voice's input sounds only while the voice is open (BT silent until its pad was
  hit; with the init kit's infinite decay it then stays open).
- MIDI while streaming (assumption 7): universal device inquiries over USB MIDI
  answered (F0 7E 00 7C 00 F7) during a 5-minute stream.
- long runs: 300 s and 120 s - the first 30-60 s with 0 counter gaps, then bursts
  of gaps (1152 in 300 s; 494 in 120 s with 32 reads in flight), no short or bad
  packets, no errors. The Mac was loaded (load average 6.7: the emulator window,
  other agents), so a user-space libusb loop missing blocks is the likely cause,
  not shown. The Teensy's own gap counter (the host line) decides on Monday.
