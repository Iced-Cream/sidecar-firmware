# usb=mirror: the box itself on the USB-C side (prototype)

Current state (2026-11-04): code only. It builds for the Pi 4 and its descriptor parser passes the portable tests. It has
not run in the emulator or on a unit. Pi 4 only (the Pi 5 builds without it).

`usb=mirror` on the card's cmdline.txt (with `gadget=sidecar`, the default) replaces the Sidecar's own USB-C device
(1209:0004, UAC2 12x12 + MIDI) with the attached box as the computer would see it plugged in directly. The aim is that
the box maker's own software (the Overbridge engine and plugins, the transfer tool) works through the Sidecar, while the
Pi keeps its own Overbridge stream with the box and adds to it.

The mode presents the box maker's USB IDs to the computer, so it is for the owner's own use. It stays opt-in and is never
on in a public card or build default.

## How it works

- **Descriptors.** When the box is configured on the USB-A side (`ob_usb.cpp`), its device and configuration descriptors,
  plus every string descriptor they name, are read into RAM (`MirrorAttach`). The USB-C gadget connects only then, and
  serves them unchanged. None of the box's bytes are in the repo: the unit test's descriptor is made up (`tests/test_mirror_desc.cpp`).
- **Endpoints** (`mirror_desc.h`). Each endpoint address the configuration names gets one gadget endpoint, sized to the
  largest `wMaxPacketSize` of any alternate setting. Packets of a smaller alternate are short packets, so nothing is
  resized at SET_INTERFACE. Roles:
  - vendor-class `0x83` / `0x03`: the audio stream;
  - every other vendor-class endpoint: a pipe (the async channel);
  - MIDI streaming: the MIDI port.
- **Audio IN.** Each IN block of the box goes to the computer as it came, cut to the channels of the computer's alternate (`ob::block_bytes`).
- **Audio OUT.** The computer's OUT blocks are decoded into the same OUT ring the UAC2 device feeds. `ToRytm` adds them to what the engine sends. The box stays the clock.
- **Alternate settings.** The computer's SET_INTERFACE is kept on the gadget side and not forwarded. The box stays at the alternate the engine opened (alt 3), because the Pi's own stream needs it.
- **Pipes.** Each packet is relayed to the same endpoint on the box. An OUT packet is NAKed to the computer until the box
  has taken it. The box's IN request is renewed only once the computer has the last packet.
- **MIDI.** Same path and filters as the default mode (`midi_filter.h`): the host link stays between the Pi and the box,
  and `usbmidi=os:` still decides whether OS upgrades pass.
- **Class and vendor requests.** Each one is forwarded to the box (`CObUsb::MirrorControl`). EP0 NAKs until the box
  answers, then returns the answer, or a STALL if the box stalls or is gone. One request is in flight at a time.

## Circle change

`pi/circle/circle-gadget-mirror.patch` (applied by `pi/engine/build.sh` after `circle-gadget-reset.patch`; it defines
`CIRCLE_GADGET_MIRROR`) makes two changes:

1. **IN and OUT endpoints can share a number** (`0x83` + `0x03`). Circle kept one endpoint object per number. An OUT endpoint now goes to a second table when an IN endpoint already has its number. A gadget without shared numbers (the default Sidecar) never uses that table.
2. **A class or vendor request can be answered later.** The new hook is `OnControlRequest() -> ControlDeferred`, completed by `CompleteControl()`, with `OnControlAborted()` called when the host moves on. The default returns `ControlNotHandled`, so other gadgets take the old path unchanged.

## Known limits of the prototype

- **Box gone.** The gadget stays connected when the box goes away, and requests are then STALLed. There's no soft disconnect yet. A box with other descriptors needs a Pi reboot.
- **No forwarding of the computer's alt or interface choices.** Alternate settings stay on the gadget side, so a vendor request that only works at another box alternate is not handled. Requests that change the box's stream state also reach the box and may disturb the Pi's own stream.
- **One control request at a time.** A second one while the first is in flight is STALLed.
- **Untested on hardware [I].** Interrupt endpoints on Circle's DWC2 gadget, the RX FIFO headroom for 368 + 512 + 512 B OUT, and the latency (about one block each way) are all unmeasured.

## Test plan (needs a go)

1. **Emulator gate.** Boot the engine with `usb=mirror` and `link=tcp`. Check that it does not start the gadget without a box, and that the default mode is unchanged.
2. **Unit run.** Pi on `usb=mirror`, the Rytm on stock 1.74, the PC on USB-C. Pass when all of these hold:
   - the PC sees 1935:0B48;
   - the Overbridge engine finds the Rytm and the plugin syncs;
   - 12 channels play both ways;
   - the transfer tool lists files;
   - elkflash `info` works through the mirrored MIDI port.

   The Pi log's `mirror` line counts every path.
