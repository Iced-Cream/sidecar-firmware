# Rytm MKII USB: modes, Overbridge protocol, bandwidth (research notes)

Date: 2026-10-07. Web research and source reading only; nothing was built or run.
Tags: **[C]** = confirmed in a cited source; **[I]** = inference (reasoning shown).

Sources used (local clones read at these commits):
- Overwitch, github.com/dagargo/overwitch @ `40018bbd71e5` (2026-09-29), GPL-3.
- Elektroid, github.com/dagargo/elektroid @ `ce8126e86e84` (2026-10-06).
- USBHost_t36, github.com/PaulStoffregen/USBHost_t36 @ `0463fc1f6591` (2026-03-11).
- Analog Rytm MKII User Manual, OS 1.74 (updated 2026-09-24):
  https://www.elektron.se/wp-content/uploads/2026/10/Analog-Rytm-MKII-User-Manual_ENG_OS1.74_260930.pdf
  (linked from https://www.elektron.se/en/support-downloads/analog-rytm-mkii). Line refs below are `pdftotext -layout` lines, page numbers are printed pages.
- Overbridge User Manual 250415: https://www.elektron.se/wp-content/uploads/2025/04/Overbridge-User-Manual_250415.pdf

Note: the current manual on elektron.se is OS 1.74 (released 2026-09/10). We target 1.73; the 1.73->1.74 notes list only an Outbox 8 main-out routing addition ("[MKII] Added the option to route the audio from the Main outputs using Outbox 8", https://www.elektron.se/release-notes/analog-rytm-mkii-os-release-notes). [C]

---

## 1. USB modes (USB CONFIG)

- Three mutually exclusive modes: OVERBRIDGE, USB MIDI, USB AUDIO/MIDI. "Selecting one mode disables the other two modes." Rytm manual 14.8.2 USB CONFIG, p. 68. [C]
- USB PIDs (vendor 0x1935 Elektron), from Overwitch `udev/91-usb-elektron.hwdb`: [C, as a community table]
  - `0x0B48` (and older `0x0010`) "Analog Rytm MKII Overbridge"
  - `0x0011` "Analog Rytm MKII MIDI"
  - `0x1048` "Analog Rytm MKII Audio/MIDI"
  - Overwitch's device table keys the Rytm MKII on pid 2888 = 0x0B48 (`res/devices.json:198-199`). [C]
- **Class-compliant (USB AUDIO/MIDI) audio is stereo, not 12 channels.** The manual describes only an L/R pair each way: [C for L/R wording; I for "exactly 2 channels" since no descriptor dump was found]
  - USB IN (host -> Rytm): PRE-FX, POST-FX, TRACK 1-12 or "L:1-12/R:1-12" ("the incoming audio's left and right signal is then sent to separate tracks"), SAMPLER ONLY. Mono-summed if one track is chosen. Manual 14.6 AUDIO ROUTING, p. 65-66 (txt lines 3566-3583).
  - USB OUT (Rytm -> host): MAIN OUT, TRACK 1-12 or L/R track pair, AUDIO IN, OFF (lines 3595-3607).
  - Sampler SRC options "USB L+R / USB L / USB R" (13.1.5 SRC, lines 3100-3106).
  - Tracks sharing a voice (3/4, 7/8, 9/10, 11/12) cannot be split L/R (line 3626).
  - Audio into a track only passes when the track's envelope is open: "you must place a note trig on the selected track(s) ... set the trig's LEN parameter to INF" (lines 3586-3589).
  - So the belief "12 from the Rytm in class-compliant mode" is wrong: 12 outputs is Overbridge, not class compliant. Class compliant = 2 in + 2 out (+ USB MIDI). [I, strong]
- Converters 48 kHz / 24-bit; "Electrically isolated USB 2.0 port" (manual 18, p. 76, line ~4085); product page: "USB 2.0 Hi-Speed port" (https://www.elektron.se/en/analog-rytm-mkii). [C]
- No public `lsusb -v` dump for the Rytm MKII was found. Only partial: USBHost_t36 issue #20 (2018, "Elektron Rytm and Analog Four", Teensy 3.6 host) shows `new_Device: 12 Mbit/sec`, `Config data length = 342`, `bNumInterfaces = 5`, interfaces `bInterfaceClass=255` (https://github.com/PaulStoffregen/USBHost_t36/issues/20). [C] The 12 Mbit/s speed means that unit was an MKI (the Overbridge manual says MKI is Full Speed, MKII Hi-Speed), probably in Overbridge mode; do not take it as MKII data. [I]
- Class-compliant sample format/alt settings for MKII: not found. The Linux kernel `sound/usb/quirks*.c`, `implicit.c` have no Elektron (0x1935) entries (checked torvalds/linux master), i.e. no quirks needed. [C for absence; I that it is plain UAC2]

## 2. Overbridge (as implemented by Overwitch)

### Device and endpoints [C]
- VID `0x1935` (`src/overwitch.h:27`), Rytm MKII PID `0x0B48`, protocol "version 2" (`res/devices.json:197-200`).
- "Version 2 (interrupt transfers) uses 4 bytes integers and is used by classic Overbridge 2 devices"; version 1 (isochronous) is MKI only (`README.md:507-512`, `docs/adding.md:62-67`).
- `src/engine.c:34-45`:
  ```c
  #define OBX_USB_CONFIGURATION 1
  #define OB2_USB_AUDIO_OUT_EP 0x03          // host -> device
  #define OB2_USB_AUDIO_OUT_INTERFACE 2
  #define OB2_USB_AUDIO_OUT_ALT_SETTING 3
  #define OB2_USB_AUDIO_IN_EP (OB2_USB_AUDIO_OUT_EP | 0x80)   // 0x83, device -> host
  #define OB2_USB_AUDIO_IN_INTERFACE 1
  #define OB2_USB_AUDIO_IN_ALT_SETTING 3
  #define OB2_USB_CONTROL_INTERFACE 4
  #define OB2_USB_MIDI_INTERFACE 5
  ```
- Init sequence (`engine.c:621-735`, `ow_engine_init_v2`): detach kernel drivers from IF 4 and 5; set configuration 1; claim IF 1, alt 3; claim IF 2, alt 3; clear_halt EP 0x83 and 0x03; allocate transfers; re-attach kernel drivers to IF 4/5; read wMaxPacketSize of each EP via `libusb_get_max_alt_packet_size` (`engine.c:707-719`) and require it to equal the computed block length, else `OW_USB_UNEXPECTED_PACKET_SIZE` (`engine.c:535-555`).
- Transfers: `libusb_fill_interrupt_transfer` on both EPs (`engine.c:431-467`). One transfer in flight per direction; each is resubmitted from its own completion callback (`engine.c:363-429`).
- Vendor control requests (`engine.c:1183-1262`): IN, vendor, device, bRequest 1 and 2, 32 bytes = the device's Overbridge name; OUT bRequest 1 sets the name. A 100 ms sleep follows "to not send the next packet immediately, which can make devices crash" (`engine.c:1224`).
- MIDI in Overbridge mode is on IF 5, left to the OS driver (Overwitch re-attaches it). [C for the code; I that IF 5 is a standard USB-MIDI interface on MKII]

### Packet format [C]
- `src/engine.h:25-150` (protocol comment, written for the Digitakt, generalised by the JSON track table): "All values are big-endian (MSB first)! Sample rate is 48kHz". A transfer is N blocks; each block:
  ```c
  struct ow_engine_usb_blk_v2 {     // engine.h:218-224
    uint16_t header;                // 0x07FF host->device, 0x0700 device->host
    uint16_t frames;                // BE sample counter, +7 per block
    uint8_t private[OB2_PRIVATE_LEN];   // 28 bytes, unknown; Overwitch sends zeros
    int32_t data[];                 // 7 frames x N tracks, interleaved, int32 BE
  };
  ```
- `OB2_FRAMES_PER_BLOCK 7` (`overwitch.h:30`); blocks per transfer 6..24, default 24 (`overwitch.h:44-46`; README says 24 x 7 = 168 frames by default, `README.md:380`; low values "might cause the device to crash", `README.md:384`).
- Host writes the header once (`htobe16(0x07ff)`, `engine.c:595`) and per block the counter (`engine.c:197-198`). Samples: float * INT32_MAX, `htobe32` (`engine.c:206-215`); reading is the inverse with `be32toh` (`engine.c:119-133`). Version 2 = full 32-bit words (Rytm converters are 24-bit, so low byte presumably 0). [I for the low byte]

### Rytm MKII channels (`res/devices.json:197-305`) [C]
- **Device -> host ("output_tracks"), 12 x 4 B:** Main L, Main R, BD, SD, RS/CP, BT, LT, MT/HT, CH/OH, CY/CB, Input L, Input R.
- **Host -> device ("input_tracks"), 12 x 4 B:** Main L Input, Main R Input, Main FX L Input, Main FX R Input, BD Input, SD Input, RS/CP Input, BT Input, LT Input, MT/HT Input, CH/OH Input, CY/CB Input.
- So stock Overbridge already carries **8 per-voice mono streams host -> Rytm**, plus main and main-FX stereo pairs. The Overbridge manual agrees: ASIO output table "Analog Rytm Output" = MAIN L/R, MAIN FX L/R, BD..CY/CB = 12 (Overbridge manual Appendix D.2, p. 47); "four stereo (L/R) inputs" per plugin with destinations "Main L, Main R, Main FX L, Main FX R or Track 1-12" (6.12 INPUT ROUTING, p. 30). Its input table lists 10 (no AUDIO IN L/R) while Overwitch has 12; Overwitch's count matches the wire. [C, I for "matches the wire": Overwitch checks packet size against wMaxPacketSize]
- Caveat: per-voice Overbridge audio enters the voice before its amp: "when you route audio from your DAW into a track or voice output on Analog Rytm MKI/MKII ... you have to make sure that the VCA in the corresponding Synth/Drum Track is triggered" (Overbridge manual 5.x, p. 10, txt line 392). Appendix B.1 draws the injection point in the voice chain. [C]

### Sizes for the Rytm [I, computed from the above]
- Frame = 12 x 4 = 48 B each way. Block = 4 + 28 + 7 x 48 = **368 B** each way (= wMaxPacketSize, by Overwitch's check). 24-block transfer = 8832 B = 168 frames = 3.5 ms (the engine.h comment's "24 blocks (total 8832 Bytes)" for the 12-channel Digitakt in-direction agrees).
- 48000/7 = 6857 blocks/s -> 2.52 MB/s (20.2 Mbit/s) per direction.
- 368 B packets exceed the Full-Speed interrupt max (64 B) and 20 Mbit/s exceeds 12 Mbit/s, so this mode only works at High Speed. At HS, one 368 B packet per 125 us microframe (8000/s) covers 6857 blocks/s, so the device must NAK some polls; packets are paced by the device. [I]

### Clocking [C/I]
- The device is the clock master. Overwitch timestamps each completed device->host transfer and feeds a DLL (`dll_overbridge_update(dll, frames_per_transfer, time)`, `engine.c:147-151`) and resamples host audio to the device rate with libsamplerate (`engine.c:224-320`). Host->device transfers are simply resubmitted on completion, so the device pulls at its own rate. [C code; I interpretation]
- For a Teensy: keep one IN and one OUT interrupt transfer queued, produce 7 frames per block on demand, and derive the Teensy's audio rate from the IN block rate (or run the Teensy audio clock from it). [I]

## 3. Elektroid (Elektron SysEx protocol)
- All device communication goes over MIDI (ALSA rawmidi / RtMidi backends, `src/backend_alsa.c:74-120`), not USB bulk. [C]
- Message = `F0 00 20 3C 10 00` + 7-in-8 encoded payload + `F7` (`src/connectors/elektron.c:208`, `:509-520`). Encoding: per 7 data bytes, one leading byte with their MSBs, then the 7 bytes & 0x7F (`elektron.c:477-505`). [C]
- Commands are request IDs (`elektron.c:210-270`), e.g. 0x01 ping, 0x02 version, filesystem 0x10-0x43, data 0x53-0x5D (Rytm uses the data filesystem; device id 16 "Elektron Analog Rytm MKII", `res/elektron/devices.json:95-110`). [C]
- Relevance: a patched Rytm could add a new request ID/handler in this SysEx protocol for control data; it inherits the 8/7 encoding overhead and USB-MIDI framing below. [I]

## 4. Bandwidth math [I, arithmetic]
USB-MIDI 1.0: each 4-byte event packet carries 3 SysEx bytes (CIN 0x4/0x5-0x7). Data bytes are 7-bit; Elektron-style 7-in-8 packing costs 8/7.

| stream | raw B/s | SysEx B/s (x8/7) | USB B/s (x4/3) | Mbit/s |
|---|---|---|---|---|
| 1 x 48 kHz 16-bit mono | 96,000 | 109,714 | 146,286 | 1.17 |
| 8 x 48 kHz 16-bit mono | 768,000 | 877,714 | 1,170,286 | 9.36 |
| 8 x 24-bit | 1,152,000 | 1,316,571 | 1,755,429 | 14.0 |

- Full-Speed bulk theoretical max: 19 x 64 B per 1 ms frame = 1.216 MB/s for the whole bus. 8 x 16-bit SysEx needs 96% of it: not feasible at FS. One or two streams would fit.
- High-Speed bulk theoretical: 13 x 512 B per 125 us = 53 MB/s. USB is not the limit; the Rytm's MIDI input path was the suspected bottleneck; measured since in rytm-emu (measure-emulator.md): the stock OS takes 1.6 MB/s of unknown SysEx at about 1 instruction a byte (the USB controller itself is not modelled there).
- The Rytm MKII port is High Speed: Overbridge manual Appendix C ("Analog Four MKII, and Analog Rytm MKII ... are USB Hi-Speed devices, with a maximum bandwidth of 480 Mbit/s", p. ~45; "can always run all channels at the highest bitrate", p. 10), product page "USB 2.0 Hi-Speed", and the 368 B interrupt packets above. [C]
- Compare: Overbridge host->device already moves 12 x 32-bit channels (2.52 MB/s) with no 7-bit or MIDI framing.

## 5. Extending Overbridge / custom data
- Nothing public found about adding channels or custom data to Overbridge on the device side. Overwitch's track tables are user-editable JSON (`docs/adding.md:11`), so a host-side Overwitch or Teensy can use a changed layout if patched firmware changes the frame layout and the endpoint wMaxPacketSize. [C for Overwitch; I for the approach]
- Elektron itself changed counts by firmware: Digitakt II / Digitone II use "version 2.1" with 3-byte samples and 42 device->host tracks (`res/devices.json`, entries pid 2859/2868; `README.md:511`). [C]
- The 28-byte `private` area in every block is undocumented and unused by Overwitch (zero-filled host->device). It is a candidate side channel for control data (Rytm -> Teensy trig/param info, 28 B x 6857/s = 192 kB/s each way) if patched firmware fills/reads it. Unknown whether stock firmware uses it. [I]
- Overwitch "is based on the Overbridge USB reverse engineering done by Stefan Rehm in dtdump" (https://github.com/droelfdroelf/dtdump; Overwitch `README.md:10`; see also Elektronauts thread https://www.elektronauts.com/t/overwitch-a-jack-client-for-overbridge-devices-aka-overbridge-for-linux/153983). [C]

## 6. Teensy 4.1 host notes
- USBHost_t36 supports HS (480 Mbit) host with control, bulk and interrupt pipes; isochronous is not implemented (`ehci.cpp:1273` "TODO: skip past iTD, siTD when/if we support isochronous"; pipe types `USBHost_t36.h:220`; HS interrupt bandwidth scheduling `ehci.cpp:1114-1135`). Overbridge v2's interrupt endpoints therefore fit the library; UAC2 class-compliant audio (normally isochronous) would not without new host code. [C code; I conclusion]
- `enumbuf` is now 2048 B (`enumeration.cpp:47`), so large Elektron config descriptors no longer overflow (issue #20 was at 256 B). [C]
- Stock `MIDIDevice` refused the Elektron interfaces because they reported class 255 (issue #20); a custom driver claiming IF 1/2 (audio) and IF 5 (MIDI) by VID/PID is needed. [C for the issue; I for the remedy]

## Open questions
- Exact MKII descriptors in each mode (need `lsusb -v` of a real unit, or read them from the firmware's descriptor tables in the emulator).
- Endpoint bInterval of 0x03/0x83 on MKII.
- What the 28 private bytes carry device->host on the Rytm.
- Where in the voice chain host->device voice audio is injected (pre-filter or pre-amp) on the Rytm specifically.

## Addendum (2026-10-07): Elektron on USB speeds [C, Elektron, quoted by the user]

"Overbridge for Analog Four MKI, Analog Keys, and Analog Rytm MKI uses USB 2.0 Full
Speed. This USB specification limits the data transfer rate to max 12 Mbps. Later
Elektron products use USB 2.0 High Speed and have a max rate of 480 Mbps. Many
laptops use a single USB host controller that controls all ports. ... multiple
devices connected that run with USB 2.0 Full Speed ... might be sharing the 12
Mbps maximum bandwidth." (Elektron support text on sharing a USB host controller.)

For the Teensy [I]: the Rytm MKII is a High Speed device, and the Teensy 4.1's host
port (the RT1062's second controller, 480 Mbit/s) would serve it alone, so there
is no bandwidth sharing when the Rytm is plugged straight in. Through a hub, the
bandwidth is shared with whatever else is on it; a Full Speed device behind a
High Speed hub goes through the hub's transaction translator and does not slow the
High Speed traffic.
