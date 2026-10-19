# Overbridge pass-through to a computer: feasibility spike (2026-10-16)

Goal: a computer sees the Rytm's Overbridge channels (12 in / 12 out, 48 kHz), and ideally the expander's own voices as extra channels. The expander (Teensy 4.1 or Pi 4) stays the Rytm's USB host.

This spike is research only. Nothing was built, uploaded or sent to the Rytm.

Tags: [source] = read in code or docs (path or link given), [inferred] = reasoning not yet tested, [measured] = measured earlier (cited).

## Summary

- Route A, class-compliant **USB Audio Class 2 + USB MIDI** on the expander's device port, is feasible on both platforms. **Teensy first.** Its core already has isochronous IN/OUT endpoints, an explicit feedback endpoint, SOF interrupts and the composite Serial+MIDI+Audio pattern (all UAC1 today). Circle's Pi 4 gadget stack has bulk endpoints only, so isochronous support would have to be written into its DWC2 driver first.
- Route B, a **transparent Overbridge proxy**, is technically possible: the Teensy's device port can do 368-byte high-speed interrupt endpoints. It is fragile and legally grey, and it cannot add the expander's voices as channels. Not recommended.
- Windows caveat: the in-box UAC2 driver supports more than 8 channels only outside shared mode (WASAPI exclusive or ASIO). macOS, Linux and iPadOS have no such problem [source/inferred, below].

## What is being forwarded (recap)

- Rytm Overbridge (`rytm-emu docs/OVERBRIDGE.md`) [measured: hardware]:
  - Interface 1 alt 3: IN interrupt endpoint 0x83. Interface 2 alt 3: OUT endpoint 0x03. Both 368 B, bInterval 1 at high speed.
  - A block is 7 frames of 12 x 32-bit samples. 6857 blocks/s each way.
  - The Rytm is the clock master. OUT is paced by IN.
  - MIDI is on interface 5.
- UAC2 at high speed, 24-bit in 3-byte subslots: 12 ch x 3 B x 6 frames = 216 B per 125 µs microframe nominal, 252 B with the +1 frame of async slack.
  - The expander's 8 voices as extra channels make 20 ch: 420 B per microframe at 3 B, 560 B at 4 B.
  - Every case fits one isochronous transaction (at most 1024 B), so no high-bandwidth (Mult) endpoints are needed.

## 1. Pi 4 + Circle 51.1.1

### Gadget support exists on the Pi 4B USB-C port [source]

- Circle 51.1.1 (`pi/circle/build/circle-engine`, HEAD 7a81e1b "Merge branch 'hotfix-51.1.1'") has `lib/usb/gadget`:
  - `CDWUSBGadget` is documented in `doc/classes.txt` as "DW USB gadget on Raspberry Pi (3)A(+), Zero (2) (W), 4B".
  - The sample `test/usb-serial-cdc-gadget/README` says the Pi 4B uses its USB-C power connector for gadget operation.
  - Circle's online USB docs still list gadget mode only for "Pi 1-3 and Zero" ([USB subsystem docs](https://circle-rpi.readthedocs.io/en/latest/subsystems/usb.html)). The source is newer than those docs.
- Gadget classes: `CUSBCDCGadget` (serial CDC ACM), `CUSBMIDIGadget` (USB MIDI 1.0) and `CUSBMSDGadget` (mass storage). There is **no audio gadget**, and no CDC-NCM or ECM network gadget, which the pi-engine web-UI plan wanted.
- Each gadget is a separate subclass of `CDWUSBGadget` with its own descriptors. There is no composite framework, so UAC2+MIDI (+NCM later) would be one new subclass with a hand-written composite descriptor.
- **Endpoints are bulk only.** `CDWUSBGadgetEndpoint` hard-codes `DWHCI_DEV_EP_CTRL_EP_TYPE_BULK` (`lib/usb/gadget/dwusbgadgetendpoint.cpp:129`).
  - Isochronous needs: the endpoint type, even/odd microframe selection (SETEVENFR/SETODDFR), the incomplete-isochronous interrupts, and per-microframe re-arming.
  - DMA mode is already on (`dwusbgadget.cpp:228`).
- FIFOs [source]: RX 512 words, 7 dedicated TX FIFOs of 384 words (1536 B) each (`dwusbgadget.cpp:35-38`). This is enough for 560 B per microframe IN.

### DWC2 gadget and VL805 xHCI host at the same time: likely yes, untested [source + inferred]

They are separate controllers:
- Separate registers: DWC2 at `ARM_USB_BASE`; the xHCI sits behind PCIe.
- Separate interrupts: `ARM_IRQ_USB` = GIC SPI 73, while the xHCI uses the PCIe INTA/MSI lines (`include/circle/bcm2711int.h`).
- `CUSBController` has no static singleton (`include/circle/usb/usbcontroller.h`).
- On the Pi 4 `CUSBHCIDevice` is `CXHCIDevice` (`include/circle/usb/usbhcidevice.h`), so the host stack never touches DWC2.
- FreeBSD's Pi 4 notes say the same: all host ports are on xHCI, and the DWC OTG drives the Type-C port in device mode ([FreeBSD D25203](https://reviews-dev.freebsd.org/D25203)).

No Circle example or forum post running both together was found, so this needs a test (step P1 below).

### Work estimate for a UAC2 (12-20 in / 12 out) + MIDI gadget

| piece | effort |
|---|---|
| isochronous IN/OUT and an interrupt/iso feedback endpoint in `CDWUSBGadgetEndpoint`/`CDWUSBGadget` (Circle patch, carried like the xHCI patches) | 4-6 days, the main risk (DWC2 iso in device mode is notoriously finicky) |
| composite descriptor: IAD, AC with one clock source (fixed 48k: GET CUR + GET RANGE), 2 AS interfaces (alt 0 empty, alt 1 24-bit), feedback EP, MIDI streaming interface; EP0 class requests | 3 days |
| FIFO glue to `pi/engine`'s Overbridge IN/OUT, feedback from the Overbridge IN rate | 2 days |
| coexistence test with the Rytm on xHCI, power test (USB-C also powers the Pi) | 1-2 days |

About 2-3 weeks. Adding NCM to the same composite later is another class implementation.

### Clock, latency, CPU [inferred]

- **Clock.** The Rytm is the master.
  - IN (to the computer): an async endpoint whose packet size is 6 frames ±1 per microframe, taken from the Overbridge IN FIFO level.
  - OUT (from the computer): an explicit feedback endpoint at high speed (16.16 frames per microframe). Its value is the Overbridge IN frames counted over 2^N SOFs, plus a small FIFO-level correction.
  - Two different clocks: Overbridge frames counted against the gadget's SOF.
- **Latency.** About 1-2 ms of FIFO each way on the device, on top of the host OS buffer. Comparable to the engine's own `fifo=96` (2 ms).
- **CPU.** Circle handles every IRQ on core 0 (`doc/multicore.txt`). Core 0 already takes the xHCI interrupt, and would add about 8-24 k gadget interrupts/s at bInterval 1.
  - The copy cost is trivial: about 1 MB/s.
  - The interrupt rate is the cost to measure. bInterval 2 (250 µs) halves it.
  - Cores 1-3 stay as they are.

## 2. Teensy 4.1

### Two controllers at once: yes [source + measured]

- USB1 (the micro-USB device port) and USB2 (the host port, USBHost_t36) are separate i.MX RT1062 controllers.
- The host build already runs the Serial console on USB1 while it hosts the Rytm on USB2 [measured, docs/teensy-host-rytm.md].

### Core USB audio today [source: Teensyduino 1.62 `cores/teensy4`]

- UAC1, stereo, 16-bit, 44.1 kHz.
- `usb.c` already has:
  - `usb_config_rx_iso` / `usb_config_tx_iso` (`usb.c:927,936`);
  - SOF interrupts (`usb_start_sof_interrupts`, `usb.c:402`);
  - high-speed detection (`usb_high_speed`).
- `usb_audio.cpp` already runs an explicit feedback endpoint: 4 bytes in 16.16 at high speed, 3 bytes in 10.14 at full speed (`usb_audio.cpp:57-109`).
- Composite USB types with Serial exist (`USB_MIDI_AUDIO_SERIAL`, `usb_desc.h:773-807`). **Serial + MIDI + audio coexist in one composite device**, and the console keeps working.
- So a UAC2 device needs:
  - a new USB type in `usb_desc.h`/`usb_desc.c`: UAC2 descriptors with an IAD, clock source, 12-20 ch 24-bit async IN, 12 ch async OUT and feedback;
  - UAC2 EP0 class requests (clock GET CUR/RANGE, optional SET CUR);
  - an N-channel ring-buffer driver;
  - the low-level endpoint code stays as it is.
- The core files are installed globally (`~/Library/Arduino15/.../cores/teensy4`). Ship this as a patched core copy (a custom `hardware/` platform or a `build.core.path` override in `run_sketch.sh`), not as edits to the installed one.

### Memory and bandwidth [inferred]

- **ITCM.** Teensy 4 places code in ITCM by default, and the host build has about 2.3 KB of ITCM headroom (`docs/teensy-host-rytm.md:129`). Every new function (descriptor handling, class requests, ring copy, feedback) must be `FLASHMEM`.
  - The per-microframe callbacks then run from flash through the I-cache. That is fine at 8 kHz, but check the interrupt timing.
  - The core's own `usb_audio.cpp` must not also be linked: a new USB type replaces `AUDIO_INTERFACE`.
- **RAM.** Rings in DMAMEM (RAM2): at about 512 frames, 12 ch x 4 B = 24 KB OUT and 24-40 KB IN. Check against the build's RAM2 headroom before choosing sizes. A 256-frame ring halves it.
- **CPU/DMA.** The ChipIdea controller DMAs from dTD buffers, so the CPU only copies about 1 MB/s plus a bit of reformatting (32-bit Overbridge samples to 24-bit). Negligible next to the voices.

### Community work

h4yn0nnym0u5e's Teensy cores/Audio branches add 2-8 USB channels and 44.1/48/96 kHz, still UAC1-style ([PJRC forum](https://forum.pjrc.com/showthread.php?p=327016); [cores branch](https://github.com/h4yn0nnym0u5e/cores/tree/feature/multi-channel-USB)).
- The author reports UAC2 "not working right now" and feedback glitches on USB 2.0.
- Tested on Windows only.
- Useful as a reference for the plumbing, not as a base.
- The Teensy core licence is MIT plus a PJRC clause: a build system listing targets must include PJRC boards. That clause is harmless here.
- No working Teensy UAC2 multichannel implementation was found ([forum, 8 ch](https://forum.pjrc.com/threads/55024-Teensy-8-Channel-Audio-Interface), [forum, 4 ch](https://forum.pjrc.com/threads/61119-Quad-channel-audio-input-through-USB-on-T3-6-or-T4-1)).

### Work estimate

| piece | effort |
|---|---|
| UAC2 composite descriptor (Serial + MIDI + UAC2) in a patched core; enumerates on a Mac | 2 days |
| N-ch 24-bit async IN/OUT driver, feedback from a rate source | 3 days |
| host-build integration: Overbridge IN -> UAC2 IN (+ voice channels), UAC2 OUT -> Overbridge OUT, feedback from the measured Overbridge IN rate vs USB1 SOF; ITCM/RAM gate | 3 days |
| Mac/Linux/Windows/iPad tests, long-run drift test | 2-3 days |

About 2 weeks.

## 3. Host OS support

| OS | UAC2 class driver | notes |
|---|---|---|
| macOS | built in (CoreAudio) | multichannel async with explicit feedback works for class-compliant interfaces; no channel limit relevant at 12-20 [inferred, widely used, not checked in an Apple doc] |
| Windows 10 1703+ / 11 | `usbaudio2.sys` in-box ([Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers)) | async IN/OUT; **async OUT needs an explicit feedback endpoint (no implicit)**; packet size within ±1 frame of nominal; one clock source only; **more than 8 channels not supported in shared mode**: DAWs need WASAPI exclusive or an ASIO wrapper; MIDI in a UAC2 device enumerates through `usbaudio.sys` as a separate function |
| Linux | `snd-usb-audio` | full UAC2 including explicit feedback [inferred]; no Elektron quirks needed (docs/research-rytm-usb.md:36) |
| iPadOS | built in (class compliant) | UAC2 multichannel works in apps like AUM [inferred]; the iPad must power the Teensy (fine) or the Pi (marginal: a Pi 4 needs about 1-1.5 A, so a powered hub or splitter) |

The design rules that follow:
- one fixed 48 kHz clock source;
- explicit feedback;
- 24-bit in 3-byte subslots (or 4);
- for Windows, either at most 8 out channels in a second alt setting or plain documentation that ASIO is needed.

## 4. Route B: transparent Overbridge proxy (assessment only)

What it would take:
- The device port presents the Rytm's exact identity: VID 0x1935, PID 0x0B48, the 263-byte configuration with 6 interfaces. The descriptor bytes may be committed.
- Interfaces 1 and 2 forward the 368-B high-speed interrupt blocks both ways. The Teensy's USB1 can do 368-B interrupt endpoints at bInterval 1.
- Vendor EP0 requests (bRequest 1/2, the 32-byte device name; Overwitch, docs/research-rytm-usb.md:57) go to the Rytm and back.
- Interface 4 (control) and interface 5 (MIDI) are relayed.

Risks:
- **Unknown protocol.** Interface 4's protocol (plugin parameter sync, total recall) is undocumented. Overwitch does not implement it, so a proxy must relay it blind, with timing the driver may check.
- **Firmware updates.** Elektron's software path (Overbridge, Transfer/C6 via MIDI) can push OS updates. A proxy would have to recognise and block them, or risk bricking the Rytm through a man-in-the-middle.
- **Fixed channel layout.** The layout is fixed at 12/12, so the expander's voices cannot be added. They could only replace or mix into existing channels.
- **Driver checks.** Overbridge driver updates may change version checks and break the proxy silently.
- **Identity.** Using Elektron's VID/PID on our hardware is impersonation of their USB identity: a trademark and USB-IF grey zone, and not fit to publish.
- **Latency.** At least one block of latency, plus timing coupling. The driver expects the Rytm's own pacing, and the proxy must never stall the host link.

Effort: weeks, with an open-ended debug risk. **Not recommended.** Revisit only if users must keep Elektron's plugins.

## 5. Recommendation and step plan

**Teensy first.** It is the main path, and its core already has iso endpoints, feedback and composite Serial. Its controllers are already proven concurrent with the Rytm hosted. The Pi needs a Circle DWC2 isochronous patch before anything else.

Teensy steps:
1. **T1 (first milestone, about 2 days): "Teensy composite Serial + UAC2 2-ch 48 kHz/24-bit enumerates on the Mac and plays a test tone; a loopback sends the Mac's OUT back on IN."**
   - A standalone sketch (`teensy/uac2_probe`) with a patched core copy. No USB host code, and the Rytm is not involved.
   - Check in Audio MIDI Setup, `system_profiler SPUSBDataType`, and the feedback value against SOF in the Serial console.
   - This prototype is cheap and safe, but it needs an upload. It replaces the running voiceloop build, which would end the current Rytm session. **Not run in this spike, upload only on approval.**
2. **T2:** 12 in / 12 out, feedback from the internal clock; long-run drift and glitch counters on macOS and Linux.
3. **T3:** host build. Overbridge IN goes to UAC2 IN, UAC2 OUT goes to Overbridge OUT, and the feedback follows the measured Overbridge IN rate. Gate ITCM (all new code `FLASHMEM`) and RAM2.
4. **T4:** the 8 voices as channels 13-20 of IN; USB MIDI forwarding of the Rytm's interface-5 MIDI; Windows (WASAPI exclusive / ASIO) and iPad tests.

Pi steps (after T3):
1. **P1:** a Circle kernel with `CUSBMIDIGadget` on USB-C plus `CXHCIDevice` with the Rytm on USB-A. Proves coexistence and power, using existing classes only, about 1 day.
2. **P2:** an isochronous patch to Circle's DWC2 gadget, then **"Pi USB-C gadget enumerates as a 2-ch UAC2 device on the Mac"**.
3. **P3:** a UAC2+MIDI composite at 12-20 ch, fed from `pi/engine`'s Overbridge FIFOs on core 0; measure the IRQ load. Fold the planned NCM web UI into the same composite later.

Licences: Teensy is MIT+PJRC. Circle is GPLv3, already accepted for the Pi image. Copy no Overwitch (GPL-3) or Elektron code; use the wire facts only, as before.

## 6. Results: T1 (2026-10-16)

Built and run on the Teensy 4.1 and a MacBook (macOS 26.7) with the Rytm on the Pi. All [measured] unless tagged.

### What was built

- `teensy/uac2/teensy4-1.62.0-uac2.patch`: new USB type `USB_SERIAL_UAC2` for Teensyduino 1.62.0's `cores/teensy4` (`usb_desc.h`/`usb_desc.c` descriptors, `usb.c` hooks for SET/GET_INTERFACE, the clock's CUR/RANGE requests and SOF).
- `teensy/uac2/usb_uac2.c`/`.h` (our code, MIT): the streaming driver. All of it is `FLASHMEM`; the buffers and the 512-frame OUT ring are `DMAMEM`.
- `teensy/setup_uac2_core.sh` builds the patched copy in `teensy/cores-uac2/` (git-ignored; the installed core is not touched). `RMP_CORE=uac2 teensy/run_sketch.sh uac2_probe` selects it (`build.core.path`, `build.usbtype`, and the precompiled-header hook pointed at the copy).
- `teensy/uac2_probe`: IN ch1 is a 1 kHz sine at -6 dBFS, IN ch2 loops back OUT ch1. It logs once a second over Serial. Its debug commands `f<ppm>` (feedback offset) and `c<ppm>` (device clock offset) were used below.
- `teensy/uac2/uac2_test.py`: a full-duplex PortAudio test, 440 Hz at -12 dBFS on OUT ch1.

Design:
- one internal fixed clock (id 0x10, 48 kHz; GET CUR/RANGE answered);
- 2 in / 2 out, 24-bit in 3-byte subslots, no feature units;
- iso async OUT 0x04, explicit feedback 0x84 (4 B, bInterval 4) and iso async IN 0x85;
- data endpoints at bInterval 2 (12 frames per 250 µs, at most 13 = 78 B);
- 4 transfers queued per data endpoint.

The device clock is the CPU cycle counter (12500 cycles per frame). The feedback base is that clock measured against FRINDEX over 8192 microframes, plus a correction toward an OUT ring level of 48 frames (about 1 s time constant, clamped at ±0.5 %). For T3 the clock is a weak function (`usb_uac2_clock_q16`) that the Overbridge rate replaces.

### Results

- **Enumeration.** "Rytm Expander UAC2" (16c0:048a, bcdDevice 0x0280) at 480 Mb/s. Its interfaces: CDC 0/1 (Serial keeps working, uploads still reboot through it), AudioControl 2, AS 3 and 4.
  - `system_profiler SPAudioDataType`: Input Channels 2, Output Channels 2, Current SampleRate 48000.
  - The host sends 3 clock GET requests and no SET CUR. No bad requests.
- **SOF.** 8000 SOF interrupts per second, FRINDEX agreeing, no missed SOFs.
- **Clocks.** The Teensy crystal against the Mac's USB clock: base 0x0005fffd..0x0005fffe, i.e. -2.5..-5 ppm (16.16 resolution is 2.5 ppm). In 660 s, 27 seconds counted 8001 SOFs: 4-5 ppm.
- **IN (Teensy to Mac).** 4000 packets/s, 48000 frames per Teensy second, sizes clamped only at start-up (3).
  - **Not checked at the Mac.** This process has no microphone permission: every CoreAudio input, built-in mic included, reads zero from here and from the terminal panel. The tone's frequency and level and the loopback stay unverified. Rerun `teensy/uac2/uac2_test.py` after allowing the app in System Settings > Privacy > Microphone: it checks the tone against its 48-entry table and the loopback bit-exactly, with latency.
- **OUT (Mac to Teensy)**, checked on the Teensy:
  - OUT ch1 at -12.0 dBFS peak, 440 zero crossings/s;
  - the largest second difference is 6988 every second, as expected for that sine (a dropped packet gives about 10^6);
  - ch2 silent;
  - no bad lengths, no dTD errors.

### macOS feedback finding (important for T2-T4)

With the feedback in 16.16 frames per microframe (what the USB 2.0 spec describes), macOS **ignored it completely** at data bInterval 2:
- Every OUT packet had exactly 12 frames over 11 min (2.6 M packets), while the ring crept from 48 to 212 at the clocks' 5 ppm difference.
- Feedback offsets of ±2000 ppm and ±2 %, at stream start or mid-stream, changed nothing.
- A device clock 2000 ppm off (IN rate following it) changed nothing either: the ring overran, then underran. So macOS also does not use the IN stream as implicit feedback.
- The host does poll the feedback endpoint: 1000 reads/s.

Two working fixes:
- **bInterval 1**, with the value per microframe: macOS follows it exactly. At -2 % it sends 47059 frames/s for 47058 asked, at +2 % 48720 for 48720 asked.
- **bInterval 2** with the value in **frames per packet** (12.0 in 16.16): macOS follows it the same way (47057 for 47061.6, 48720 for 48719.7). This is the shipped default: half the interrupt rate.

Linux auto-detects the feedback format [inferred, snd-usb-audio]. **Windows is unknown: test in T4.**

### 11-minute run, final configuration (bInterval 2, feedback per packet)

660 s full duplex (PortAudio, 480-frame blocks, 31.68 M frames played):
- **Rates.** OUT packets 2,637,017 of 12 frames, 238 of 11 and 119 of 13: the host tracking the -5 ppm device. The feedback moved between 47999.02 and 48000.73 Hz.
- **Ring.** The OUT ring stayed at **47-49 frames** after the first 4 s (target 48).
- **Errors.** Underruns 0, overruns 0, bad lengths 0, dTD errors 0. PortAudio reported no xruns.
- **Glitches.** 659 of 660 seconds had zc=440 and d2max=6988. The only glitch was the stream start.
- **Latency.** The OUT ring adds about 1 ms. The IN queue is 4 packets (1 ms) [inferred from the design, not measured: needs the Mac recording].

Build:
- ITCM 35208 B for the standalone sketch.
- The UAC2 driver adds no ITCM. Every `usb_uac2_*` function sits at 0x6000xxxx (nm). The only ITCM growth is the core's small `usb.c` hooks.
- RAM2: 19 KB used in total.

### Problems and open points

- IN audio not verified on the Mac (microphone permission, above).
- Start-up: one discontinuity on OUT ch1 as the stream starts. The ring primes to 48 frames before it is read; the first packets are clamped.
- At stream stop one underrun is counted (the host stops sending OUT before IN). Harmless.
- Full-speed descriptors exist (bInterval 1, 49 frames, 3-byte 10.14 feedback) but are untested.

### Step to T2

`-DUAC2_CH_IN=12 -DUAC2_CH_OUT=12` are already parameters of the patch. The step:
1. Make the in/out-fill hooks and the ring N-channel.
2. Check packet sizes: 13 x 12 x 3 = 468 B, fine for one transaction.
3. Rerun this 11-minute test on macOS and Linux, with the Mac's microphone permission granted so the IN side and the loopback are checked bit-exactly.
4. Test the feedback format on Windows.

## 7. Results: T2 (2026-10-16)

All [measured] on the Teensy 4.1 and the MacBook (macOS 26.7), unless tagged. The Mac app now has microphone permission, so both directions are checked end to end. Configuration: bInterval 2, feedback per packet.

### What changed

- **Channel counts are build options.** `RMP_EXTRA_FLAGS="-DUAC2_CH_IN=12 -DUAC2_CH_OUT=12"` (T2) and `"-DUAC2_CH_IN=20 -DUAC2_CH_OUT=12"` (the T4 target: Rytm 12 + 8 voices). The default stays 2/2.
- **Descriptors.**
  - More than 2 channels get bmChannelConfig 0 (raw, numbered channels).
  - A high-speed packet over 1024 B is a build error: at most 13 frames x 26 channels x 3 B.
  - Full speed is capped at 1023 B per packet, so more than 2 channels need high speed.
- **`uac2_probe`, IN (to the Mac).**
  - Ch 1..N-2 carry sines of distinct frequencies at -12 dBFS. The periods divide 48000 (100, 120, 150 ... 960 Hz), and the table is computed in double so the Mac can check every sample exactly.
  - Ch N-1 loops back OUT ch 1, ch N loops back OUT ch M.
- **`uac2_probe`, OUT (from the Mac)**, checked per channel: zero crossings per second, peak, largest second difference, and discontinuities. A discontinuity is a second difference above twice what a sine of the previous steady second could make.
- **`teensy/uac2/uac2_test.py`** reads the channel counts from the device.
  - It plays a prime-frequency sine on every OUT channel (1009, 1103 ... 2099 Hz). Every period is 1 s, so the loopback lag is unambiguous.
  - It checks every IN sample against its table, and the loopbacks bit-exact with the round-trip lag. It ends with PASS or FAIL.
  - `--selftest NI NO` checks the checker on synthetic input with an injected dropped packet.
  - `SECS FILE.npy` also keeps the recording.
  - **One command verifies everything:** `uv run --with sounddevice python -I teensy/uac2/uac2_test.py 660` (with microphone permission for the app that runs it).
- **PortAudio I/O is float32.** PortAudio's float-to-int32 conversion scales by 2^31-1 and truncates, so in int32 every positive 24-bit sample arrived 1 LSB low. That was measured: about 83 % of positive samples, negatives exact, Mac-to-Teensy exact. It is a host-library artefact, not the device. In float32 the 24-bit values come through exactly.

### Results

| run | enumerates (CoreAudio) | IN (Teensy to Mac) | loopback (Mac to Teensy to Mac) | OUT on the Teensy | counters |
|---|---|---|---|---|---|
| 12/12, 660 s | 12 in / 12 out, 48000 | 10 tone channels: every sample matches over 660.2 s (31.69 M frames), right channels, -12.04 dBFS peak | OUT ch1 and ch12: **bit-exact** over 660.2 s, round trip 1660 frames (34.6 ms, PortAudio buffers included) | 12 channels at 1009..2099 Hz, -12 dBFS; **0 discontinuities** on every channel | ring 47-49 frames; feedback 47999.02..48000.73 Hz; OUT packets 2,781,142 x 12 frames, 193 x 11, 69 x 13; underruns 0 while streaming, overruns 0, bad lengths 0, dTD errors 0; PortAudio xruns 0 |
| 20/12, 660 s | 20 in / 12 out, 48000 | 18 tone channels (100..960 Hz): every sample matches over 660.4 s | bit-exact, 1660 frames | 12 channels, 0 discontinuities | ring 47-49; feedback 47998.90..48000.61 Hz; OUT 2,679,076 x 12, 258 x 11, 59 x 13; 0 errors; xruns 0 |
| 2/2 (T1 re-check), 20 s | 2 / 2 | ch1 1000.000 Hz; matches after the start-up transient below | bit-exact, 1660 frames | | |

Further measurements:
- **Build size.** The 20/12 build takes 37,896 B of ITCM (the standalone sketch); the driver itself is all FLASHMEM. The OUT ring is 512 frames x 12 ch x 4 B = 24 KB in RAM2.
- **CPU cost.** The per-sample ISR work (12-20 channels at 48 kHz, plus the OUT checks) caused no SOF loss: 8000 SOF/s throughout.

### Open points

- **Start-up transient (2 of about 10 stream starts).** About 1.33 s (63,550 and 64,080 frames) of unmatched samples at the very start, with a raised rms. It hit one tone channel in the first 20/12 run and the 1 kHz channel in the 2/2 re-check. The rest of each run was exact (0 breaks).
  - Both were the first stream after a re-enumeration with a new channel count.
  - Provoking it again three times (reboot; 12 then 20; 20 then 12) gave clean starts.
  - Cause unknown: CoreAudio settling on a new device is a guess. T3 must look at stream start-up (Rytm sessions start and stop often).
- **Mac-side mixing event (12/12 run without permission).** One simultaneous discontinuity on all 12 OUT channels, after which the levels the Teensy received changed and stayed changed (the same frequencies).
  - The USB counters showed nothing: no bad lengths, underruns or overruns.
  - It was about when microphone permission was granted. The content changed on the host side, likely a CoreAudio reconfiguration or a second client [inferred].
  - Not seen in the two later 11-minute runs.
- **Windows feedback format.** Not testable here; research only:
  - Microsoft's usbaudio2.sys documentation ([USB Audio 2.0 Drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers)) states the requirements: explicit feedback only (no implicit) for async OUT, packet sizes within ±1 audio slot of nominal, one clock source, and more than 8 channels only outside shared mode. It does not give the feedback format.
  - The Linux UAC2 gadget driver (`drivers/usb/gadget/function/u_audio.c`, `u_audio_set_fback_frequency`) shifts the high-speed value by `bInterval - 1` with the comment "Win10 and OSX UAC2 drivers require number of samples per packet in order to honor the feedback value" [source]. That is the same per-packet rule we measured on macOS, so the shipped format is the likely right one for Windows too [inferred].
  - TinyUSB's comments report that Windows 10 also wants 16.16 at full speed instead of 10.14 [source, secondhand]. Our full-speed path sends 10.14, but full speed only carries 2 channels anyway.
  - To test in T4: with a Windows machine, check the packet histogram moves under `f-20000` / `f20000` (the probe's debug command), as done here.

### Step to T3

The host build:
- **Wiring.** Overbridge IN (12 ch, the Rytm's clock) goes to UAC2 IN. UAC2 OUT goes to Overbridge OUT.
- **Clock.** `usb_uac2_clock_q16()` is overridden with the Overbridge IN frame count. The IN packet sizes and the feedback then follow the Rytm's clock rather than the Teensy crystal.
- **Gates.** ITCM (the driver is FLASHMEM; check the host build's 4-block budget) and RAM2.
- **Start-up.** Look at stream start, given the transient above.
- **Retest.** Rerun `uac2_test.py` (tones replaced by the Rytm's channels, so the IN check becomes a level and continuity check) together with the host build's xrun counters.

## 8. Results: T3, the host build (2026-10-16)

Setup:
- The Rytm (rytm-mods hwus1nl build, OS 1.73) on the Teensy's USB host port, playing patterns.
- The Teensy's micro-USB on the MacBook (macOS 26.7).

All [measured].

### What was built

- **`teensy/host_uac2.sh [SECS]`**: the voiceloop host build with the patched core.
  - The Teensy is the Rytm's USB host as before.
  - It is also "Sidecar", a 12 in / 12 out UAC2 device (+ Serial) for the computer.
  - Its flags: `RMP_CORE=uac2`, `-DRYTM_USB_HOST=1 -DRYTM_NOTES=1 -DRYTM_OUT_MAIN=1 -DUAC2_CH_IN=12 -DUAC2_CH_OUT=12 -DUAC2_FB_WINDOW=65536 -DUAC2_HS_ONLY=1 -DUAC2_RING_FRAMES=256 -DUAC2_RING_ATTR=`.
  - Back to the plain host build: `RMP_EXTRA_FLAGS="-DRYTM_USB_HOST=1 -DRYTM_NOTES=1 -DRYTM_OUT_MAIN=1" teensy/run_sketch.sh voiceloop`.
- **Channel map**, as the Overbridge plugin numbers them:

| computer | is |
|---|---|
| IN 1-2 | the Rytm's MAIN L/R |
| IN 3-10 | the tracks BD SD RS BT LT MT CH CY (the Rytm's Overbridge track outputs) |
| IN 11-12 | the Rytm's AUDIO IN L/R |
| OUT 1-2 | the Rytm's MAIN L/R input |
| OUT 3-4 | MAIN FX L/R input |
| OUT 5-12 | the 8 tracks' inputs BD SD RS BT LT MT CH CY (mixed into each voice before its analog chain) |

- **Mixing toward the Rytm.** The computer's OUT is added (saturating) to what the voiceloop sends on the same channels. Two cases:
  - With `:out=tracks` the voices are on their tracks' inputs and the computer's channels add to them.
  - With `:out=main` the voices' mix is on MAIN and the computer's channels add to MAIN and the tracks.
  - Silence from the computer changes nothing, so the default behaviour is unchanged.
  - `:mac=off` / `:mac=on` (console) drops or keeps the computer's OUT. `:mac` shows the state.
- **Clock.** The Rytm is the master.
  - The UAC2 clock (`usb_uac2_clock_q16`) is the Rytm's Overbridge IN frame count, spread over the time since each transfer.
  - The feedback base is measured over 65536 microframes (8 s), because the frames arrive 7-14 at a time.
  - The OUT ring level corrects the rest.
  - IN packet sizes come from the IN ring's level (`usb_uac2_in_frames`): 12 frames, 11/13 outside ±7 frames of a 40-frame target. Sizing them from the bursty clock directly had clamped 70 % of packets, though without data loss.
  - Without a stream the clock falls back to the cycle counter.
- **Library and driver changes.**
  - RytmHost has a TX callback (`setTxCallback`, added once per OUT block in `fill_out`, the arithmetic in FLASHMEM).
  - The driver has `usb_uac2_in_frames`, `usb_uac2_in_delay` and `usb_uac2_ext_consumer`.
  - Its HS-only buffers and the RAM1 ring option save RAM2.
- **Memory.**
  - ITCM: 4 blocks, headroom 1528 B (was 2296 B: the core's `usb.c` hooks and the `fill_out` call).
  - RAM1 locals: 63,360 B (was 88,608): both 12 KB rings moved there, because RAM2 is nearly full in the host build.
  - RAM2 heap: 14,080 B left; the latency capture (`:cap`, 11 KB) still runs.
  - Program image: 3.3 MB below the plugin store.

### Results

**11 minutes with the Rytm playing** (`uac2_rytm_test.py 660`, OUT silence):
- **Mac side.**
  - PortAudio xruns 0; 31.68 M frames recorded.
  - All 12 channels present at stable levels: MAIN L/R -22.3 dBFS rms (peaks -6.6/-6.8), SD -21.8, BT -17.0, LT -29.7, MT -31.5, CY -49.4. BD, RS and CH at their noise floors (pattern dependent); AUDIO IN L/R -108 dBFS (nothing plugged).
  - **0 dropouts**, i.e. no run of ≥3 frames with all 12 channels exactly zero; the noise floors make such a run impossible otherwise.
- **Teensy, Overbridge.**
  - 0 xruns, FIFO 89..96 (target 96), 0 bad blocks, 0 counter gaps, 0 unanswered blocks.
  - 47,999 Hz in and out.
  - CPU 12.4-14.9 %.
- **Teensy, UAC2.**
  - IN 2,637,205 packets of 12 frames, 286 of 11, 4 of 13; underruns 0.
  - At this point the IN-overflow count read 294, all at stream start (then counted from the first IN packet; fixed afterwards, below).
  - OUT ring 34..68 frames, 0 underruns while streaming, 0 overruns, 0 bad lengths or errors.

**2-minute run on the final build** (overflow counting fixed, full log line):
- IN overflow 0. Feedback 47,997.68..48,002.56 Hz, base 47,999.755 Hz: the Rytm about 5 ppm slow against the Mac's USB clock.
- 8000 SOF/s. PASS.

**Mac to Rytm** (`uac2_rytm_test.py 30 --tone`): a 1009 Hz sine at -40 dBFS on OUT 1/2 for 2 s came back in the Rytm's MAIN mix (IN 1/2) at -51 dBFS, against -72 dBFS before. PASS.

**Stream start-up**, 6 short start/stop cycles:
- Mac side: no dropouts at any start.
- Teensy side: no IN loss at start or stop once overflow was counted only while IN is selected and read.
- The host stops OUT just before IN, so OUT "under" counts 1 per stop; harmless.
- The T2 start-up transient (about 1.3 s, CoreAudio, after a re-enumeration with a new channel count) was not seen.

**Unchanged behaviour** (on this build):
- The mod's EXT trigs play the voices.
- Host-link SysEx flows: in 933 after a minute.
- MIDI clock 131 BPM.
- The console still answers `:notes`, `:out=tracks` / `:out=main`, `:fifo` (target 96), `:cap=200` (9600 frames, 0 lost) and unknown commands.
- Plugin store mounted, 8 installed.

**Gate.**
- `teensy/gate.sh quick`: PASS (serial build, sizes, golden renders).
- Core tests (`core/ make test`): ALL PASS.
- `sim/rmp_e2e.py` (serial): all pass except test 8's "a trig while a save runs". It expects the card's current project to be "PRESETS"; this card now holds the user's Rytm project ("AR PERC PATCH", from their session), so the slot file name differs. The held time was in range (4.6 ms). The serial build contains no UAC2 code, so this is not a T3 regression.
- `rmp_e2e.py --host`: not run (needs the Rytm unplugged).

### How to use it

1. Run `teensy/host_uac2.sh`. "Sidecar" appears in Audio MIDI Setup as 12 in / 12 out at 48 kHz.
2. Record the Rytm's MAIN on inputs 1-2 and the tracks on 3-10, as with Overbridge.
3. Play into outputs 1-2 (MAIN) or 5-12 (a track's input, which goes through that voice's analog chain).
4. **Warning:** macOS may make Sidecar its default output when it appears. System sounds then go to the Rytm's MAIN input. Pick another output in macOS, or use `:mac=off`.

### Step to T4

- **Voices on the computer.** Channels 13-20 of IN carry the 8 voices: a `UAC2_CH_IN=20` build, tested in T2. The voices' frames go to the IN ring next to the Rytm's 12, taken from the same ring frames `host_feed` pushes. Mind RAM1: about 6 KB more ring.
- **USB MIDI.** Forward the Rytm's interface-5 MIDI as class-compliant USB MIDI in the composite device (BACKLOG).
- **Windows.** WASAPI exclusive or ASIO for more than 8 channels; check the per-packet feedback with `f±20000`.
- **iPad.**
- **Host e2e.** Rerun `rmp_e2e.py --host` with the Rytm unplugged.

## 9. Pi P1/P2 results (2026-10-16)

Raspberry Pi 4 on Circle 51.1.1 (`pi/circle/build/circle-engine`), its USB-C port to the MacBook (macOS 26.7), which also powers it. The normal engine runs throughout; the VL805 xHCI host on USB-A is initialised as before. The Rytm was not connected (it was on the Teensy for T3). All [measured] unless tagged.

### What was built

- **`pi/circle/circle-gadget.patch`** (Circle, applied by `pi/engine/build.sh`, idempotent; checked with a dry run on a clean 51.1.1 tree). It touches only `lib/usb/gadget` and `include/circle/usb/gadget`:
  - The gadget's IRQ count and the time spent in it (`GetIRQStats`).
  - `CUSBMIDIGadget::GetMIDIDevice`. The gadget's MIDI device also takes a `umidiN` number, so the engine now takes the Rytm's as the first `umidiN` that is not the gadget's (it was hard-wired `umidi1`).
  - USB MIDI IN transfers capped at 508 B (see the problems below).
  - **Isochronous (and interrupt) endpoints** in `CDWUSBGadgetEndpoint`. They get the endpoint type, one packet per (micro)frame (MC=1) and the even/odd frame bit from a target (micro)frame. The first transfer waits for the host's first token to the disabled endpoint (OUT: "OUT token while disabled", IN: NAK), which gives the host's phase; the target is that (micro)frame plus the interval. After a completion it is the last target plus the interval.
    - A target that passes unserved (the incomplete isochronous IN/OUT interrupts) disables the endpoint until the next token. This is the Linux dwc2 buffer-DMA scheme, written from the register behaviour, no Linux code.
    - An OUT endpoint is disabled under the global OUT NAK.
    - "EP disabled" no longer panics on an isochronous endpoint.
    - The next-EP learning sequence is kept for bulk endpoints only.
  - EP0: `SET_INTERFACE` and `GET_INTERFACE` reach the gadget class (`OnSetInterface`, `OnGetInterface`). Before, `SET_INTERFACE` was acknowledged silently and `GET_INTERFACE` stalled.
  - OUT DMA buffers are invalidated in the cache after the transfer as well. Without this the 660 s loopback had 56 breaks, because the CPU read lines it had fetched while the DMA was writing.
- **`pi/engine/gadget.{h,cpp}` (P1).** `CSidecarMIDIGadget`: Circle's MIDI gadget with the strings "Sidecar"/"Sidecar" (1209:0001, the pid.codes test PID configured in the tree). It echoes every event packet back bit-exact, and its stats line carries the IRQ load and the firmware's throttled flags.
- **`pi/engine/uac2_gadget.{h,cpp}` (P2).** Our own UAC2 function, with the Teensy T1 descriptor layout minus CDC:
  - IAD, AC with one internal fixed 48 kHz clock (GET CUR/RANGE), AS OUT interface 1 with EP 0x01 (78 B), explicit feedback EP 0x82 (4 B, bInterval 4) and AS IN interface 2 with EP 0x83.
  - 2 ch, 24-bit in 3-byte subslots, async. Data endpoints at bInterval 2, feedback in 16.16 frames **per packet** (the T1 macOS finding).
  - The device clock is the generic timer: IN packets of 11..13 frames as it makes them due. The feedback is that clock against the host's microframe numbers over 8192 microframes, plus a correction toward an OUT ring level of 48 frames (about 1 s, clamped at ±0.5 %).
  - IN ch 1 is the 1 kHz −6 dBFS table that `uac2_test.py` checks for 2 channels; IN ch 2 loops back OUT ch 1.
- **Selection.** `gadget=midi|audio|0` in cmdline.txt, or built in with `GADGET=audio pi/engine/build.sh` (the serial bootloader's card is not edited from the Mac). The default is `midi`.
- **Tests.** `pi/engine/tests/gadget_midi_test.py` (mido/python-rtmidi) checks the MIDI echo. `teensy/uac2/uac2_test.py` takes `UAC2_NAME="Pi audio probe"` to pick the Pi's device by exact name, never the Teensy's.

### Results

- **P1 enumeration.** "Sidecar", manufacturer "Sidecar", 1209:0001, 480 Mb/s, 500 mA allocated (`system_profiler SPUSBHostDataType`; macOS 26 has no `SPUSBDataType`). It shows as the CoreMIDI port "Sidecar".
- **P1 MIDI echo.** Each run of `gadget_midi_test.py` checks three things:
  - round-trip latency (500 single notes);
  - 10-60 s of mixed traffic: notes, CCs, pitch bends and SysEx up to 300 B at about 750 messages/s;
  - a 2000-note burst.
  - PASS every time after the fixes below, for example 44,621 messages (807 KB) all back in order, the burst back at 68-88 k messages/s.
  - The round trip is 5.4-5.5 ms median (p99 10.6 ms) at the Mac. The Pi echoes in its IRQ, so this is Mac-side CoreMIDI/USB scheduling [inferred, not isolated].
- **Host coexistence.** The xHCI host on USB-A still enumerates by plug-and-play with the gadget up: the hub 2109:3431 was found at 1.76 s on every boot. The Rytm was not available, so the Overbridge stream with the gadget running is still untested.
- **IRQ load on core 0.**
  - MIDI gadget: 0 IRQ/s idle; up to 1634/s (0.36 % of core 0) under the mixed MIDI load.
  - UAC2 probe while streaming: 13,000 IRQ/s, **1.44 % of core 0**. That is 4000 OUT + 4000 IN + 1000 feedback completions. The other ~4000/s are most likely the incomplete-isochronous interrupts of the microframes between targets, which return at once [inferred].
  - Cores 1-3 untouched: the engine's fifo stayed at its target, `under 0`.
- **Power.** The firmware's throttled flags (`GET_THROTTLED`, read without clearing) stayed `0x0` (no under-voltage, now or since boot) through a 60 s MIDI load with `selftest=on drone=all` and both 11-minute audio runs. Temperature was 47-52 °C. No brown-out or reset in about 2 h on the Mac's port.
- **P2 enumeration.** "Pi audio probe" (manufacturer Sidecar, 1209:0002), 480 Mb/s.
  - CoreAudio: 2 in / 2 out, 48000 Hz.
  - 3 clock GET requests, no bad requests, SET_INTERFACE on both AS interfaces.
- **P2 660 s full duplex** (`uac2_test.py 660`, PortAudio float32, 480-frame blocks): **PASS**.
  - IN ch 1: every sample of the 1 kHz table matched over 660.1 s (31.69 M frames).
  - IN ch 2: a bit-exact loopback of OUT ch 1 at one constant round trip of 1624 frames (33.8 ms, PortAudio buffers included).
  - PortAudio flags 0.
  - On the Pi, the 150 s + 660 s runs together:
    - OUT packets 3,239,965 × 12, 930 × 13 and 2 × 11 frames: macOS follows the per-packet feedback;
    - feedback 48001.1-48002.2 Hz, i.e. the Pi's crystal against the Mac's USB clock is +23..+46 ppm;
    - OUT ring 47-48 frames once settled;
    - overruns 0, bad lengths 0, underruns 1 per stream stop;
    - 1 incomplete per endpoint per stream start (the phase search).

### Problems found (fixed)

- **USB MIDI IN transfers of exactly 512 B** end without a short packet. macOS's open read then kept the last messages of a burst until more data came; they turned up in the next session. Fixed by the 508 B cap.
- **macOS 26 sends MIDI-CI** (universal SysEx `F0 7E xx 0D …`) to a MIDI device when a port opens. An echoing device sends it back. A random test SysEx that happened to be `F0 7E … 0D` wedged CoreMIDI's port: input stopped, then output.
  - The test now uses the non-commercial ID 0x7D and drops CI replies.
  - For P3: never forward a host's MIDI-CI back to it.
- **The iso patch's first version** had three bugs:
  - DMA to Circle's 4-byte dummy buffer (memory corruption, an exception);
  - parity flipping on every incomplete interrupt (80 % of core 0 in IRQ);
  - no hold on the host's phase for interval > 1.
  - The token-based start fixed all three.
- **A signed/unsigned slip in the ring correction** drove the feedback to +0.5 % and the ring to full: 156 k overrun frames in the first 11-minute run. Fixed.

### Open points

- **IN clamps.** About 1 IN packet in 9000 is armed too late for its service interval (292 in 810 s). The packet is skipped and the next ones carry 13 frames.
  - No sample is lost (the tone matched every sample), but the host sees a short gap in packet timing.
  - Likely an IRQ latency over 250 µs on core 0 [inferred]. Candidates: the engine's own work on core 0, or Circle's logger/serial.
  - For P3: count the late re-arms (`GetIsoLateCount`) and find what holds core 0's interrupts.
- **Composite.** Circle still has no composite framework. Each gadget is one `CDWUSBGadget` subclass with its own descriptors, so MIDI and audio are separate builds here.
  - Since P2's class is our own, a UAC2+MIDI composite is one more interface pair in the same descriptor plus a MIDI endpoint class of our own (`CUSBMIDIGadgetEndpoint` is tied to `CUSBMIDIGadget`). This is not a blocker [inferred].
- **Device name.** The UAC2 probe is not called "Sidecar" on purpose. `uac2_test.py` (and T3's tooling) picks the first CoreAudio device whose name contains "Sidecar" or "UAC2", and the Teensy's T3 build is also "Sidecar".
  - Before P3 names the Pi's composite "Sidecar", the Teensy tools must select by exact name or channel count.
- **Untested.** Full speed (the gadget is high-speed only); Linux and Windows hosts; the Rytm streaming on USB-A while the UAC2 gadget streams.

### State left

The Pi runs the normal engine with the default build: `gadget=midi`, the "Sidecar" MIDI echo on USB-C, which is harmless (0 IRQ/s idle). It was set to `notes=0 out=tracks drone=off` over the console. `gadget=0` turns the port off and `gadget=audio` selects the P2 probe.

### Step to P3

- One composite `CDWUSBGadget` class: UAC2 at 12 in / 12 out, later 20 in, plus USB MIDI.
- IN is fed from `pi/engine`'s Overbridge IN (the Rytm's clock replaces the generic timer in `ClockQ16`), and UAC2 OUT goes to Overbridge OUT.
- Packets: 13 × 12 × 3 = 468 B, and 780 B at 20 ch, both within one transaction and the 1536 B TX FIFOs.
- Measure:
  - core 0's IRQ load with the xHCI Overbridge interrupts added;
  - the late re-arms;
  - stream start/stop with Rytm sessions.
- Then rename to "Sidecar" once the Teensy tools select exactly.

## 9. Results: T4, voices and USB MIDI (2026-10-16)

The same setup as T3: the Rytm playing (the user also had Ableton open on Sidecar's inputs, and stopped and restarted the sequencer once). All [measured] unless tagged.

### What was built

**`teensy/host_uac2.sh`: Sidecar is now Serial + USB MIDI + UAC2 20 in / 12 out.**

| computer | is |
|---|---|
| IN 1-12 | the Rytm, as in T3 |
| IN 13-20 | the Teensy's 8 voices, in the same track order as IN 3-10 (BD SD RS BT LT MT CH CY) |
| OUT 1-12 | the Rytm's Overbridge inputs, as in T3 |

- **The voice channels** carry each voice's own signal as rendered: after its level, before the MAIN mix and its limiter, also with `:out=main`.
  - `host_feed` copies the frames into a 256-frame ring before `main_mix` (RAM1, 4 KB). The UAC2 interrupt reads it 160 frames behind (3.3 ms, set by measurement: 96 underran).
  - After a start or an underrun the reader sends silence until the margin is there again.
- **USB MIDI** is the core's `usbMIDI` on MIDI interface 2 (UAC2 moved to 3-5). Its code is FLASHMEM and its buffers in RAM1 (`-DUSB_MIDI_BUF_ATTR=`).
  - **Rytm to computer:** everything (notes, CC, PC, pitch bend, pressure, clock, transport, song position, SysEx) except our host-link SysEx F0 7D 52 54, which stays between the Rytm and the Teensy.
  - **Computer to Rytm:** channel messages, clock and transport; SysEx only when complete and neither ours nor Elektron's (F0 00 20 3C: OS updates and dumps never reach the Rytm this way). All from `loop()`, merged with the voiceloop's own host-link sends as whole messages.
  - The first 64 messages to the Rytm (then every 1000th) and the first 16 blocked SysEx are logged.
- **Memory.** ITCM 4 blocks, headroom 1320 B. RAM1 locals 48,192 B (the 40 KB floor holds). RAM2 heap 17,856 B.
- **Web console** (`web/`): a "Computer audio" row (`:mac=on|off`, Show), with the mock updated.

### Bugs found on the way

- **Voice block not compiled.** `UAC_VOICES` was defined after `usb_uac2_in_fill`, so the voice block was compiled out: the voice channels were silent and the ring ran full.
- **A read-order race emptied the ring.** "Is the computer reading IN" read the cycle counter before `uac_last_fill`. When the UAC2 interrupt came in between, the difference wrapped to "long ago" and the ring was emptied: about 94 voice underruns in 11 minutes, which looked like jitter until the fill gaps were measured (at most 271 µs).
- **CYCCNT wraps.** Its 7.16 s wrap also made a long idle look recent. All three idle and start checks now use `millis()` (read before it is compared), with a 1-2 ms threshold.

### Results

**11 minutes, the Rytm playing** (`uac2_rytm_test.py 660`, PortAudio xruns 0):
- **Mac side.** All 12 Rytm channels present (MAIN -7.7 dBFS rms, BD -5.9, SD -22.4, the rest at their floors in this pattern), 0 dropouts.
- **Voice channels.** vBD -13.0, vSD -33.7, vRS -24.6, vBT -11.0, vLT -22.7, vMT -20.0, vCH -18.9, vCY -45.0 dBFS rms. All 8 voices were triggered by the mod's EXT trigs during the run (trig counts in the Teensy log: 22..175 per voice in 25 s). CH and CY are silent between notes (44 % and 72 % zero samples).
- **Teensy.** The serial log of this run was cut by another client taking the port. The counters since boot (read after the run) cover it:
  - IN 2,708,887 packets: 2,708,378 x 12 frames, 426 x 11, 83 x 13; IN ring under 0 / over 0.
  - Voice ring: 2 underruns since boot (the earlier 2-minute runs on this build: 0).
  - USB MIDI to the computer: 39,925 messages.
  - Overbridge: xruns 0, FIFO 75..96.
- **The previous build's run** (before the race fix) had the full log:
  - CPU 30.2-33.4 %, peak block 53 %.
  - Feedback 47,996.2..48,015.9 Hz.
  - SOF 8000/s.
  - Overbridge xruns 0, FIFO min 75.

**MIDI** (mido / python-rtmidi):
- **Rytm to Mac:** the clock at about 50 messages/s (the Rytm at 131 BPM) and one universal SysEx (7E 00 7C); no host-link SysEx came through.
- **Mac to Rytm:** a note on/off on channel 1 (note 60) was sent through Sidecar and passed to the Rytm (`usb midi: to the Rytm #5: 90 ch 1 60 100`, `#6: 80 ch 1 60 0`). Whether the Rytm sounds it depends on its MIDI channel settings [not observed].
- **Other Mac clients:** another client (Ableton most likely) sent 2-4 messages to the Rytm through Sidecar after each boot, and 2 SysEx that were not passed (`f0 f7`, 2 B; `f0 7e 7f 0d`, 32 B: a universal device-inquiry-style message).

**Stream start-up:** 3 + 3 start/stop cycles on the final build, counters 0.

**Unchanged:** Overbridge streaming, host-link SysEx, EXT trigs, `:notes`, `:out`, `:fifo`, `:cap` (as in T3; `:cap` relies on RAM2's heap, now 17.8 KB).

### Not done

- **`rmp_e2e.py --host`:** not run; the Rytm stayed connected.
- **Serial e2e test 8 with the PRESETS project:** not re-run. It would mean changing the card's current project away from the user's ("AR PERC PATCH"), which writes their card's state. To do it: plug in a spare card, or ask the user to switch the project to PRESETS, run `sim/rmp_e2e.py`, and switch back.

### Windows and iPad (documentation only, nothing tested)

- **Windows 10 1703+ / 11, usbaudio2.sys.**
  - Sidecar is class-compliant UAC2 (one clock source, explicit feedback, packet sizes nominal ±1). MIDI enumerates separately through usbaudio.sys.
  - More than 8 channels are not supported in shared mode. Use WASAPI exclusive mode, or an ASIO wrapper (e.g. FlexASIO / ASIO4ALL over WASAPI exclusive), to see all 20 inputs. In shared mode Windows may refuse the 20-channel input or offer none.
  - Feedback: we send 16.16 frames per packet. The Linux gadget driver states that Windows 10 needs that too [source, §7], so it should work [inferred].
  - **Check:** stream, then `f-20000` / `f20000` in the Teensy console. The `uac2:` line's OUT 11/12/13 histogram must move (short packets with -2 %, long with +2 %). If it does not, Windows wants another format: try 16.16 per microframe (one line in `fb_event`).
- **iPadOS:** class-compliant UAC2 and MIDI are supported by iPadOS [inferred]. AUM and similar apps can pick channels. The iPad must power the Teensy; the Rytm has its own supply. Untested.
- **Linux:** snd-usb-audio auto-detects the feedback format [inferred]; untested.

### Step after T4

- The runtime USB mode switch: multi / stereo / off from the console and the web page (in progress).
- Overbridge mode toward the computer later, opt-in.

## 10. The USB audio mode at runtime (2026-10-16)

All [measured] on the Sidecar build, with the Rytm connected and streaming.

### What was built

- **`:usbmode=multi|stereo|off`** (`:usbmode` shows it):

| mode | the computer sees |
|---|---|
| multi | Serial + MIDI + audio 20 in / 12 out (T4) |
| stereo | Serial + MIDI + audio 2 in / 2 out: the Rytm's MAIN L/R both ways (IN 1/2 = MAIN, OUT 1/2 = the Rytm's MAIN input) |
| off | Serial + MIDI |

- **One descriptor, patched.** The core's const configuration descriptor stays the full one.
  - `usb.c`'s GET_DESCRIPTOR (configuration and other-speed) copies it and calls `usb_uac2_config_desc`.
  - That cuts the audio function off (it is last; 3 interfaces fewer), or sets the channel counts, channel configs and data endpoint sizes for the mode.
  - The driver's channel counts are runtime values; the buffers keep the build's maximum.
  - VID/PID (16c0:048a), the serial number and bcdDevice are the same in every mode, so the serial port keeps its name.
- **Switching.** A soft detach: `usb_uac2_detach` drops the pull-up and sets `usb_configuration` to 0, so Serial and MIDI writes return at once. About 350 ms later `loop()` attaches again (non-blocking steps in `host_poll`), and the computer enumerates the new descriptor.
  - The Rytm side (the USB host controller) is not touched.
  - The serial port is back after 1.5-2.4 s.
- **Persisted.** 2 bytes in the core's emulated EEPROM (address 0x0F00: magic 0x5C, the mode; 0x607C0000+, above the plugin store's flash, never in it).
  - Read in `startup_middle_hook`, before `usb_init`, so the first enumeration after a boot already has the mode.
  - Written only when it changes. A flash write runs with interrupts off for under a millisecond.
  - An EEPROM sector erase (tens to hundreds of ms with interrupts off) happens only after about 1000 changes [inferred from eeprom.c: 2 bytes a change in a 4 KB sector].
  - The byte read and write functions were moved to FLASHMEM in the core copy (ITCM).
- **Web console** (`web/`): the USB section's Audio mode select is enabled: multi / stereo / off, with overbridge as a disabled "later, opt-in" option.
  - It sends `usbmode=...` and shows "re-enumerating…".
  - The existing auto-reconnect matches vendor and product, which are unchanged. After it reconnects the page asks `usbmode`, and the reply sets the select and the status.
  - Checked with the page's mock; the real WebSerial reconnect was not driven from here.
- **Totals line.** A new log line, `usb host: since attach xruns ... usb mode ...`: Overbridge totals since the Rytm attached (the per-line counts miss lines lost while detached).

### Bug found and fixed (host build, older than this work)

While nothing on the computer has the serial port open, log lines filled the USB serial buffers. The next write then waited 120 ms (usb_serial.c's timeout), which stalled `loop()` and emptied the Rytm's Overbridge FIFO.
- **Measured:** 265 xrun blocks after a boot, and about 1100 blocks (170 ms) per re-enumeration.
- **Fix:** the host build drops plain log lines while DTR is off. pyserial, the web console and serial monitors set DTR when they open the port. The serial build is unchanged.

### Results

- **CoreAudio follows every switch** (`system_profiler SPAudioDataType`):
  - multi to stereo: 20/12 to 2/2;
  - stereo to off: the audio device goes away (Serial and MIDI stay);
  - off to multi: 20/12 again.
- **Stereo audio:** IN 1/2 at -7.8/-7.9 dBFS rms, the same as multi's MAIN; 0 all-zero frames.
- **Overbridge totals:** after a boot with no reader for 15 s, then stereo, off and multi: xruns 0 blocks, unanswered 0, bad 0, gaps 0.
- **Persistence:** set stereo, reboot (the monitor's 'R'): the port is back after 2.8 s and CoreAudio shows 2/2 at once; `usb: mode stereo`, 0 xruns since attach. Back to multi: 20/12.
- **Build:** ITCM 4 blocks (1112 B headroom); RAM1 locals 48,192 B; RAM2 heap 17,856 B.

### Notes

- **Ableton:** the user's set had Sidecar's inputs enabled. A switch removes and re-adds the device in CoreAudio; Ableton needs its audio device re-selected after a mode change [inferred, not observed here].
- **Overbridge toward the computer** is left for later (opt-in, with a warning), as asked.

### Name (2026-10-16)

- **Product name.** The Teensy's USB product name is now "Sidecar | Expander (Mini)" (the Pi: "Sidecar | Expander (Pi)").
- **macOS changes the punctuation in one property.** It keeps the descriptor's product string unchanged (`kUSBProductString`), but derives "USB Product Name" from it with '|' and '()' replaced by '_'. CoreAudio used that derived name: "Sidecar _ Expander _Mini_" [measured].
- **Fix.** The audio function now carries the name itself: string 5, used as the IAD's iFunction and the AC interface's iInterface (and the MIDI interface's iInterface). CoreAudio then lists "Sidecar | Expander (Mini)".
- **CoreMIDI** keeps the name it stored when it first saw the device (same VID/PID/serial): the MIDI port still reads "Sidecar" on this Mac. Removing the old device in Audio MIDI Setup > MIDI Studio should bring it back under the new name [inferred; not done here, it is a Mac setting].
- **Test tools.**
  - `uac2_rytm_test.py`: picks a device named with Sidecar (Expander and Mini first). `UAC2_NAME=<exact CoreAudio name>` picks one when both Expanders are connected.
  - `uac2_test.py` (`UAC2_NAME` / `UAC2_MAKER`, the Pi work's) already handles both.
- **Web console.** The title is "Sidecar | Expander".

## 11. Defaults for the mod build, settings kept, the full gate without the Rytm (2026-10-16)

### Defaults

The Sidecar | Expander (Mini) build (`teensy/host_uac2.sh`) boots with **out=tracks and MIDI notes off**: the mod build (hwus1nl) plays each voice on its track, through the Rytm's filters and FX.
- `:out` and `:notes` are kept in the emulated EEPROM next to `:usbmode` (2 bytes at 0x0F02: magic 0x5D, bit 0 out=main, bit 1 notes) and applied at boot over the build's defaults.
- **Measured:** `:out=main`, reboot, the boot log said `output: main (...)`; `:out=tracks` again.
- **A stock Rytm** (notes play the voices, which go to MAIN): `:out=main` and `:notes=1` once (kept), or `HOST_UAC2_FLAGS="-DRYTM_OUT_MAIN=1 -DRYTM_NOTES=1"` for the defaults.
- `docs/teensy-host-rytm.md`'s stock-Rytm build line is marked as the stock-Rytm option.

### Gate, Rytm unplugged

| step | result |
|---|---|
| serial build, sizes | PASS |
| golden renders | CHECK-TEENSY PASS |
| `sim/rmp_e2e.py` (serial) | ALL PASS, with the card's current project set to PRESETS for test 8 (it expects `/.rytm/slots/PRESETS.txt`), then set back to the user's AR PERC PATCH |
| host build (`-DRYTM_USB_HOST=1`), sizes | SIZES PASS (ITCM 128,792 B, 4 blocks; locals 88,608; RAM2 free 18,112) |
| `rmp_e2e.py --host` | ALL PASS (PRESETS, then AR PERC PATCH again) |
| `rmp_e2e.py --host` on the Sidecar build | ALL PASS (PRESETS, then AR PERC PATCH again) |
| plaits_bench | ran |
| plugin-less build (`RYTM_PLUGINS=0`) | boots, ITCM 108,312 B, 4 blocks |

`teensy/gate.sh full` itself stops at test 8 while the card's project is not PRESETS. The steps after it were run by hand, as listed.

The card's current project is set by the Rytm's `10 <project>` reports and is back to AR PERC PATCH. The Rytm also sends its own project when it attaches again.

The Teensy is left on the Sidecar build: multi, tracks, notes off.

## 12. Pi P3 results: Sidecar | Expander (Pi) (2026-10-16)

The Pi 4 (Circle 51.1.1) on the MacBook (macOS 26.7) by USB-C. The Rytm (mod build hwus1nl, OS 1.73, playing patterns) is on the Pi's USB-A for the Rytm stages. All [measured] unless tagged.

### What was built

- **`pi/engine/sidecar_gadget.{h,cpp}`**: one composite device from our own descriptor (Circle has no composite framework). It replaces the P2 probe.
  - Identity: 1209:0004, product **"Sidecar | Expander (Pi)"**, manufacturer "Sidecar". Its audio function and MIDI jacks carry the same name, so CoreAudio and CoreMIDI show it in full.
  - **UAC2:** 12 in / 12 out at 48 kHz, 24-bit in 3-byte subslots, async, bInterval 2, feedback per packet (P2's layout).
    - `SIDECAR_CH_IN=20 pi/engine/build.sh` builds 20 in. Packets are then 780 B, within one transaction and the 1536 B TX FIFO.
    - Every channel is named by a string (MAIN L ... VOICE 8).
  - **USB MIDI 1.0:** interfaces 3 (AC) and 4 (MS), bulk 0x04 / 0x85, one cable.
- **Channel map: the Teensy T3/T4's**, so DAW templates work for both:
  - IN 1-2: MAIN; IN 3-10: BD SD RS BT LT MT CH CY; IN 11-12: AUDIO IN; IN 13-20 (20-in build): the Pi's 8 voices as rendered.
  - OUT 1-2: MAIN; OUT 3-4: MAIN FX; OUT 5-12: the tracks' inputs.
- **Wiring in the engine:**
  - `ob_usb.cpp`'s IN completion passes each Overbridge block to the gadget (`FromRytmBlock`).
  - `FillOut` adds the computer's OUT to the engine's Overbridge OUT, saturating (`ToRytm`); `:mac=off` drops it.
  - `FillOut` also keeps the voices for IN 13-20 (`Output::Pull` hands them out).
  - **The Rytm is the clock.** IN packets are 12 frames, or 11/13 when the IN ring is more than 7 frames off its 40-frame target. The feedback is the Rytm's frames per microframe over 65536 microframes, plus the OUT ring's correction toward 48 frames.
  - Without the Rytm: the self-test (`:selftest=on`) stands in as the source (its OUT frames come back as IN); with no source at all, the generic timer and silence.
- **MIDI pass-through** through `pi/engine/midi_filter.h` (portable; `tests/test_midi_filter.cpp`):
  - **Dropped both ways:** the host link `F0 7D 52 54`, and MIDI-CI `F0 7E|7F xx 0D`.
  - **Dropped toward the Rytm:** Elektron OS updates `F0 00 20 3C dd 00 7E|7F` (the shape measured in an OS .syx), and macOS's empty `F0 F7` on every port open.
  - The computer's messages for the Rytm are sent by core 0's loop (`CObUsbTransport::Poll`), between the engine's own replies and never inside a SysEx.
  - There is no echo mode anymore (P1's echo stays as `gadget=midi`).
- **Console** (core 0's own commands):
  - `:mac=on|off`, `:mac`;
  - `:uac2test=on|off`: IN tones and the loopbacks of OUT 1 and OUT 12, the T2 check;
  - `:midifake=on|off`: without the Rytm's MIDI, what would go to the Rytm comes back as if the Rytm had sent it.
  - Both test modes are off at boot.
- **Boot defaults** (the user's decision): `out=tracks`, `notes=off`, no drone. cmdline.txt or the console still override them (`docs/pi-engine.md`).
- **`pi/circle/circle-gadget.patch`:**
  - An IRQ latency probe: the longest gap between handler entries, the longest handler, gaps over 300 µs.
  - The time of the last late re-arm.
  - **A fix:** the TX FIFO flush when an iso IN endpoint stops is now polled. Circle's `FlushTxFIFO` waits in 1 ms `MsDelay` steps; at IRQ level that held core 0 for 1008 µs at every stream stop, and the xHCI host lost Overbridge blocks (128 in one stop).
- **Tools:**
  - `teensy/uac2/coreaudio_pick.py`: CoreAudio devices and CoreMIDI ports by exact name (and manufacturer), via ctypes.
  - `uac2_test.py`: `UAC2_NAME` picks by exact name; without it, the Teensy first.
  - `pi/engine/tests/`:
    - `sidecar_midi_test.py`: the filters and the pass-through, with `:midifake=on`, no Rytm;
    - `sidecar_audio_test.py`: the stand-in chain;
    - `sidecar_rytm_test.py`: with the Rytm. It reads MIDI only and plays at most a short tone over Overbridge audio.

### Results

- **Enumeration.** "Sidecar | Expander (Pi)" next to "Sidecar | Expander (Mini)" (the Teensy): both are visible and picked apart by exact name.
  - CoreMIDI keeps the first name it saw for a USB identity. A P3 build under a test name had to move from PID 0003 to 0004 to show the real one.
- **Test mode 12/12, 660 s, PASS.** 10 tone channels matched every sample; the loopbacks of OUT 1 and 12 were bit-exact at a constant 1624-frame round trip.
  - 20/12, 30 s: all 20 channels exact.
  - 300 s with the Rytm streaming on USB-A at the same time: PASS.
  - One earlier 300 s run had isolated breaks, while Ableton had the same device open. That is the same kind of Mac-side event as T2's.
- **MIDI without the Rytm** (`:midifake=on`): **PASS.**
  - Notes, CCs, pitch bend, clock and SysEx (0x7D, an Elektron kit-dump shape, a non-CI universal message) came back unchanged and in order.
  - Host link, MIDI-CI and both OS-update shapes were blocked.
  - 7581 mixed messages and a 1500-note burst came back all in order.
- **With the Rytm, 660 s (`sidecar_rytm_test.py 660`): PASS.**
  - **Mac side.** PortAudio flags 0, **0 dropouts**.
    - All 12 IN channels at pattern levels: MAIN -9 dBFS rms, BD -10, SD -22, BT -11, CH -11, CY -56, AUDIO IN -108 (nothing plugged).
    - MIDI from the Rytm: 9566 notes and 34655 clocks; 0 host link and 0 MIDI-CI reached the Mac (about 25,000 host-link messages were dropped on the Pi).
  - **Pi side.**
    - Overbridge 6857/s each way, gap 0 / lost 0 during the run, engine fifo 78..96 (target 96), under 0.
    - UAC2 OUT ring 30..61, IN ring 28..50, IN underruns 0. IN packets 0.9 % 11, 98.2 % 12, 0.9 % 13 frames.
    - Feedback 47,993.9..48,005.7 Hz (the Rytm's clock against the Mac's).
    - Gadget IRQ 12,700/s = 2.36 % of core 0. Gaps between gadget IRQs at most 282 µs, handler at most 6 µs, 0 late re-arms.
- **Mac to Rytm** (`--tone-db=-30`): a 1009 Hz sine on OUT 1/2 came back in the Rytm's MAIN on IN 1 at -38.9 dBFS, against -64.7 dBFS before. At -40 dBFS it drowned in the playing pattern (-47 dBFS at that frequency); the Pi's log showed it mixed in (peak 83886, -40 dBFS).
- **20-in build with the Rytm.** IN 13-20 carry the 8 voices, which the mod's trigs play (-11..-45 dBFS rms), next to the Rytm's 12.
- **Start/stop.** 8 cycles of 6 s with the Rytm after the FIFO-flush fix:
  - every start without a dropout;
  - Overbridge gap 0 / lost 0;
  - handler at most 6 µs;
  - 1 incomplete per endpoint per start (the phase search), 1 OUT underrun per stop.
  - Before the fix, each stop cost the Overbridge stream about 2 ms (16 counter gaps, 128 blocks in one stop).

### The late IN packets (P2's open point)

- **The stall that cost data: the 1 ms flush at a stream stop**, above. It was found with the IRQ probe: handler 1008 µs, twice, both at a stop, and an Overbridge IN gap of 2.07 ms at the same time. Fixed.
- **Mid-stream, no core-0 interrupt latency problem was found.**
  - Streaming, the longest gap between gadget IRQs was 160-282 µs, with 0 gaps over 300 µs in the Rytm run.
  - 0 late re-arms in 660 s with the Rytm (the xHCI interrupts on the same core) and in the test-mode runs.
- **P2's "IN clamps" were a different counter.** They counted the packet sizes clamped by the timer-based sizing (the frames made due at the completion IRQ's time), not missed packets. P3 sizes packets from the ring level and counts real misses (`GetIsoLateCount`) [inferred, from the counters' definitions].
- **The self-test's pacer slips** (its own 0.5 ms tolerance on core 0's loop) explain the stand-in check's rare breaks. They are a property of the test harness, not of the bridge.

### Open points

- **No Windows, Linux or iPad tests**, and no full-speed tests.
- **Default channel count.** The default build is 12 in. The Teensy's is 20 in; the 20-in build works here (above).
- **Two clients.** A second client of the device (Ableton next to a test) shows on the test's samples; this is CoreAudio's behaviour.
- **Writes to the Rytm.** The pass-through toward the Rytm was tested only with the fake loop. With the Rytm attached, the tests send nothing to its MIDI (the rule for these runs). The direction itself is the same code path as the fake.

### State left

- The Pi runs the normal engine with the P3 default build: `gadget=sidecar`, 12 in / 12 out + MIDI, with test modes off at boot.
- `out=tracks`, `notes=off`, drone off are now the boot defaults.
- With the Rytm on USB-A, IN 1-12 carry the Rytm's Overbridge channels.
