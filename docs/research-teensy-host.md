# Teensy 4.1 as USB host for the Analog Rytm MKII: research

Date: 2026-10-07. Web research and source reading only; nothing was built or run.

Tags: **[confirmed]** read in a source/spec, cited. **[inferred]** my reasoning from
the confirmed facts, not measured. **[to measure]** needs the real Rytm (or its
firmware's USB descriptors).

Source snapshots read (shallow clones, 2026-10-07):
- PJRC USBHost_t36 `0463fc1` (2026-03-11), github.com/PaulStoffregen/USBHost_t36
- A-Dunstan teensy4_usbhost `3604a23`, github.com/A-Dunstan/teensy4_usbhost
- TinyUSB `e42fa93` (2026-10-07), github.com/hathach/tinyusb
- Overwitch `40018bb` (2026-09-29), github.com/dagargo/overwitch

## 1. Teensy 4.1 hardware

- [confirmed] The main micro-USB port is a USB *device* at 480 Mbit/s; the separate
  USB host port is a 5-pin header ("5 Pins with power management") and "runs at
  480, 12 or 1.5 Mbit/sec, depending on the speed of the device you connect".
  https://www.pjrc.com/store/teensy41.html
- [confirmed] Host power goes "through a current limited switch and a large
  capacitor" (same page). The switch is a TI TPD3S014 (0.5 A), off until
  USBHost_t36 `begin()` turns it on: forum.pjrc.com/threads/70947 and
  https://protosupplies.com/learn/prototyping-system-for-teensy-4-1-working-with-usb-host/ ;
  in code, `USBHost_t36/ehci.cpp:208-213` drives GPIO_EMC_40 (GPIO8 bit 26) high on
  `ARDUINO_TEENSY41`. The Rytm is self-powered, so VBUS current doesn't matter
  [inferred].
- [confirmed] The host port uses the RT1062's second controller/PHY (USB2/USBPHY2):
  `ehci.cpp:170-207` brings up `CCM_ANALOG_PLL_USB2` and `USBPHY2`, and
  `ehci.cpp:236` puts the controller in host mode (`USBMODE_CM(3)`). TinyUSB's
  Teensy 4.1 BSP uses the same split, `BOARD_TUD_RHPORT=0`, `BOARD_TUH_RHPORT=1`
  (`tinyusb/hw/bsp/imxrt/boards/teensy_41/board.cmake:14-15`). Both controllers are
  ChipIdea EHCI-compatible (`tinyusb/src/portable/chipidea/ci_hs/hcd_ci_hs.c:93`
  calls `ehci_init`).
- [confirmed] Cable: PJRC's 5-pin host cable needs a soldered header on the 4.1:
  https://www.pjrc.com/store/cable_usb_host_t36.html
- [inferred] The Teensy can stay a USB device to the Mac (serial/debug, like M8
  headless) while acting as host to the Rytm, because the two ports are separate
  controllers. Forum thread on running both: forum.pjrc.com/threads/63670.

## 2. USBHost_t36

**Classes** [confirmed, `USBHost_t36.h`]: USBHub (608), USBHIDParser (692) with
Keyboard/Mouse/Digitizer/Joystick (773-994), MIDIDevice / MIDIDevice_BigBuffer
(1172-1535), USBSerial(_BigBuffer) (1541), AntPlus (1701), RawHID (1922), USBSerialEmu
(1960), Bluetooth (2216), ADK (2402), USBDrive mass storage (2508). There is no audio
class driver.

**No isochronous** [confirmed]:
- `USBHost_t36.h:220` defines pipe type 1 = isochronous, but `new_Pipe()`
  (`ehci.cpp:607-690`) only handles control/bulk (async list) and interrupt
  (periodic QH). No iTD/siTD code exists, and `ehci.cpp:1273` says "TODO: skip past
  iTD, siTD when/if we support isochronous".
- Paul Stoffregen, 2018-09-13: "USBHost_t36 only supports 3 of the 4 USB transfer
  types. Someday I'll add isochronous, but it's a *lot* of work", and on sync
  (adaptive/async/sync) "is the difficult part of supporting audio":
  https://forum.pjrc.com/index.php?threads/teensy-usb-host-audio.53751/
- 2020-12-23, asked whether he would add USB host audio: "the answer is definitely no":
  https://forum.pjrc.com/archive/index.php/t-65587.html
- In the 2026 replies to thread 53751, jmarsh says USBHost_t36 "still has no support
  for isochronous transfers. My teensy4_usbhost library does" (see §4).

**How MIDIDevice claims** [confirmed, `midi.cpp:86-221`, `enumeration.cpp:407-432`]:
- Enumeration offers every 9-byte interface descriptor (each alternate setting
  too) to every unclaimed driver, so one MIDIDevice instance claims one interface.
  It works on composite devices; a second MIDI interface needs a second instance.
- It accepts class 1 / subclass 3 (MIDIStreaming). It also takes classes outside
  2..18, such as vendor 0xFF, if they carry CS_INTERFACE 0x24 MIDI jack
  descriptors (the Yamaha case). It rejects unknown CS subtypes (`midi.cpp:146-149`),
  so a Roland/Boss-style descriptor needs a patch (forum.pjrc.com/threads/63073).
- It takes only the first IN and first OUT endpoint, which must be bulk or
  interrupt (`midi.cpp:152`). An endpoint is used only if its `wMaxPacketSize` is no
  larger than the instance's buffer (`midi.cpp:190, 201`).
- [inferred] Audio Control and isochronous Audio Streaming interfaces are rejected:
  no MIDI pipes get created, or the iso endpoint fails the bulk/interrupt check. A
  standard class-compliant Elektron MIDIStreaming interface should be claimed.
  Elektron devices aren't named anywhere in the PJRC sources or forum threads I
  found [to measure].

**Buffers and SysEx** [confirmed]: `MIDIDevice` has a 64-byte packet buffer and an
80-word queue; `MIDIDevice_BigBuffer` has 512 bytes and 400 words
(`USBHost_t36.h:1510-1535`). The library's own MIDI examples use BigBuffer
(`examples/MIDI/Basic/Basic.ino:13`). A high-speed bulk endpoint has 512-byte packets,
so a 64-byte MIDIDevice would refuse it [inferred from `midi.cpp:190`]. Incoming
SysEx is assembled in a 290-byte buffer (`SYSEX_MAX_LEN`, `USBHost_t36.h:1174`); for
longer messages use `setHandleSysEx` with the partial callback (`midi.cpp:575-598`).
Bulk qTDs are chained in 16 KB pieces (`ehci.cpp:817`).

**SysEx throughput** [inferred]: USB-MIDI 1.0 carries 3 SysEx bytes per 4-byte packet,
so one 512-byte bulk packet holds 384 payload bytes. 8 mono voices × 48 kHz × 16-bit is
768 kB/s, about 1 MB/s once encoded as 7-bit SysEx. That is far below HS bulk
capacity (tens of MB/s), so the limits are the Rytm side (its USB MIDI task, how
it schedules SysEx, CPU) and the driver's one-packet-at-a-time RX queueing
(`midi.cpp:254-259`). One or two mono channels over SysEx is a plausible prototype;
8 channels is a stretch [to measure].

**Interrupt pipes** [confirmed, `ehci.cpp:1114-1165`, `661`]: HS interrupt pipes
support intervals down to 1 microframe and bandwidth checking (≤80% per microframe),
but `QH_capabilities2(1, …)` hard-codes high-bandwidth mult = 1, i.e. at most one
packet (≤1024 B) per microframe, about 8 MB/s per pipe. This matters for Overbridge
(§4).

## 3. Existing projects

- No Teensy-hosts-an-Elektron project was found, nor any PJRC thread naming
  Digitakt/Rytm/Digitone/Syntakt with USBHost_t36. Searches: forum.pjrc.com,
  GitHub, Elektronauts.
- Generic USB-MIDI host boxes (e.g. Hobbytronics) are reported to work with
  Elektron machines: https://www.elektronauts.com/t/connecting-usb-device-to-digitakt-dawless/77046
- A Digitakt hardware mod adds a USB-MIDI host mirrored to the DIN port:
  https://www.elektronauts.com/t/digitakt-mod-with-usb-midi-host-and-internal-accumulator/158230
- Teensy 4.1 MIDI-host projects with USBHost_t36 exist, e.g. doctea/usb_midi_clocker,
  which ships a patched USBHost_t36 ("merge-interrupt-fix") and reports stuck or
  missed notes: https://github.com/doctea/usb_midi_clocker/blob/main/README.md .
  A USB-host lag report: forum.pjrc.com/threads/57347.
- USB audio host on Teensy: PJRC threads 53751 and 65587 report no working
  implementation with USBHost_t36. The only Teensy 4 EHCI host stack with
  isochronous support I found is A-Dunstan/teensy4_usbhost (§4). It has no audio
  class driver.

## 4. Alternatives and the Overbridge path

**Key finding: Overbridge 2 on the Rytm MKII uses interrupt transfers, not
isochronous.** [confirmed, Overwitch, a GPL-3 reverse-engineered implementation,
not Elektron documentation]:
- README: "Version 2 (interrupt transfers) uses 4 bytes integers and is used by
  classic Overbridge 2 devices"; Rytm MKII is `"version": "2"`, pid 2888 (0x0B48), in
  `res/devices.json`. Isochronous is used only by OB1 (Rytm MKI/A4 MKI) and the
  Tonverk.
- `src/engine.c:36-46`: audio OUT endpoint 0x03 on interface 2, alt 3; audio IN
  endpoint 0x83 on interface 1, alt 3; control interface 4, MIDI interface 5. It
  uses `libusb_fill_interrupt_transfer` (`engine.c:433, 453`).
- Block format (`src/engine.h:36-100`): a 32-byte header (0x07FF host→device / 0x0700
  device→host, BE uint16 sample counter += 7), then 7 frames × N channels × int32 BE.
  The default is 24 blocks per transfer.
- Rytm MKII track layout (`devices.json`): device→host has 12 channels (Main L/R,
  BD, SD, RS/CP, BT, LT, MT/HT, CH/OH, CY/CB, Input L/R); host→device lists 12
  (Main L/R, Main FX L/R, BD..CY/CB "Input").
  [to measure] Whether the stock firmware actually uses the 8 per-voice host→device
  channels: Overwitch only names them, and our notes (rytm-emu RESUME) say only 2
  are used today. With our firmware patch they are the natural place for "one Teensy
  channel per voice", since the slots are already in the stream.
- [inferred] Bandwidth: 12 ch × 4 B × 7 + 32 = 368 B per block, 48000/7 ≈ 6857
  blocks/s, about 2.5 MB/s each way, about 315 B per 125 µs microframe. That is well
  within one ≤1024 B interrupt packet per microframe, so USBHost_t36's interrupt
  pipes (mult = 1) are enough in principle. The endpoints' real `wMaxPacketSize` and
  `bInterval` in alt 3 are [to measure] (dump descriptors on the Mac with the Rytm in
  Overbridge mode, or read them from the firmware in rytm-emu).
- What an Overbridge host on Teensy needs [inferred from Overwitch]: SET_CONFIGURATION
  1; SET_INTERFACE alt 3 on interfaces 1 and 2; CLEAR_FEATURE(HALT) on 0x83/0x03;
  continuously queued interrupt IN/OUT transfers carrying 0x07FF/0x0700 blocks with a
  running sample counter; and clock-drift handling. Overwitch uses a DLL plus
  libsamplerate; on the Teensy the synth could instead render to the device's
  pace. Name/vendor control requests (`engine.c:1243-1252`) look optional. The
  data path is a small USBHost_t36 driver (claim interfaces 1/2 by VID 0x1935, Elektron per Overwitch `udev/91-usb-elektron.hwdb` /
  PID, new_Pipe type 3). It needs no isochronous support. Note that Overwitch is
  GPL-3, so reimplement from the documented format rather than copying code.

**Class-compliant USB audio host** (if Overbridge is rejected):
- A-Dunstan/teensy4_usbhost [confirmed]: "full support for low/full/high speed
  devices, including control/bulk/interrupt and isochronous endpoints". It runs
  on either Teensy port (`TeensyUSBHost1/2`) and has an iso API
  (`src/ehci/endpoint_iso.cpp`, `src/ehci/driver.h:76-105 IsochronousMessage`) used by
  a UVC camera example (`examples/camera/jpg_server`). It has no UAC driver. It is
  GPL-3.0 and depends on TeensyAtomThreads.
- TinyUSB [confirmed]: the Teensy 4.1 BSP and the EHCI HCD exist, and a new UAC1/UAC2
  *host* class driver exists (`src/class/audio/audio_host.c`, 2026, MIT). But the
  EHCI HCD rejects isochronous: `src/portable/ehci/ehci.c:417-418`
  `TU_ASSERT(… != TUSB_XFER_ISOCHRONOUS)` "TODO not support ISO yet", plus 450, 554,
  705, 795, 935. So it doesn't work on the RT1062 today.
- NXP MCUXpresso SDK [confirmed]: the `usb_host_audio_speaker` example (bare-metal and
  FreeRTOS) lists the MIMXRT1060-EVKB/EVKC:
  https://docs.mcuxpresso.nxp.com/mcuxsdk/25.06.00/html/examples/usb_examples/usb_host_audio_speaker/readme.html .
  The host stack has iTD/siTD support (`host/usb_host_ehci.c:296-399`,
  `USB_HOST_CONFIG_EHCI_MAX_ITD/SITD`) and UAC 2.0 definitions
  (`host/class/usb_host_audio.h`) in github.com/nxp-mcuxpresso/mcux-sdk-middleware-usb.
  The example is playback-only (host→speaker). Porting it to Teensy is
  feasible: the same EHCI, USB2 instance [inferred]. Frank B suggested this
  in thread 65587. There is also an NXP thread on host-audio receive-length
  bugs: community.nxp.com/t5/i-MX-RT-Crossover-MCUs/USB-Host-Audio-Stream-Receive-Data-Length-Misreporting/m-p/936229 .
- [to measure] How many channels the Rytm exposes in "USB AUDIO/MIDI" mode. The
  manual describes the class-compliant card as 2 in/2 out at 48 kHz, which isn't
  enough for 8 mono voices without a firmware change to the descriptors.

## 5. Emulating the Teensy

- Renode [confirmed]: has `platforms/cpus/imxrt1064.repl` and
  `platforms/boards/mimxrt1064_evk.repl` (Cortex-M7, LPUART, GPIO, GPT, ENET, FlexSPI).
  But USB (0x402E0000) and SAI are only `Tag` placeholders with no model, so a
  USB host stack can't run against it:
  https://github.com/renode/renode/blob/master/platforms/cpus/imxrt1064.repl
- QEMU [confirmed]: its i.MX machines are i.MX25/6UL/7D/8M only, with no i.MX RT:
  https://www.qemu.org/docs/master/system/target-arm.html
- **Recommendation** [inferred]: compile the Teensy-side logic natively on macOS, as
  `rytm-teensy/sim` already starts to. Put the synth (Plaits is plain C++) and
  the protocol codec behind a transport interface. Back it with CoreMIDI/virtual
  ports to rytm-emu ("rytm-emu usb in/out") for the SysEx path, or with a socket or
  in-process link carrying Overbridge-format 0x07FF/0x0700 blocks for the audio
  path. On hardware the same codec sits on USBHost_t36 MIDI or interrupt pipes.
  Instruction-level emulation of the RT1062 adds nothing here, since the USB
  controller is the hard part and no emulator models it. Check timing (CPU budget
  of Plaits ×8 at 600 MHz) on the real board.

## Practical transport path

1. Prototype: USBHost_t36 `MIDIDevice_BigBuffer` with the Rytm in USB MIDI mode.
   Trig/param messages go Rytm→Teensy, and 1-2 voices of audio return as SysEx.
   Everything here exists today.
2. Target: an Overbridge-2 host driver on USBHost_t36 interrupt pipes. It needs no
   isochronous support, and the 12-channel host→Rytm block already has per-voice
   slots for the patched firmware to consume. MIDI interface 5 coexists in that mode,
   so MIDIDevice can still carry control.
3. Only if Overbridge mode is unusable: a class-compliant UAC host via teensy4_usbhost
   (GPL-3) or a port of NXP's host audio stack, and the Rytm descriptors would also
   need more channels.
