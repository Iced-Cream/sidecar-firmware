# usb=mirror: the box itself on the USB-C side

Current state (2026-11-05): run on a Pi 4 with an Analog Rytm MKII on OS 1.74, the review changes included (see Tested below). Pi 4 only: the Pi 5
builds without it.

`usb=mirror` on the card's cmdline.txt (with `gadget=sidecar`, the default) replaces the Sidecar's own USB-C device
(1209:0004, UAC2 12x12 + MIDI) with the attached box as the computer would see it plugged in directly. The aim is that
the box maker's own software (the Overbridge engine and plugins, the transfer tool) works through the Sidecar, while the
Pi keeps its own Overbridge stream with the box and adds to it.

The mode presents the box maker's USB IDs to the computer, so it is for the owner's own use. It stays opt-in and is never
on in a public card or build default.

## How it works

- **Which device the computer sees.** The Sidecar device (1209:0004) at boot and whenever no box is there. The box once
  it has been read. The Sidecar device again after the box has been gone for `mirrorhold=` seconds (default 10, up to 600),
  so a box that reboots, as after an OS update, comes back as itself. Each switch is a replug: the gadget leaves the bus
  (`SoftDisconnect`), waits 300 ms, and comes back with the other descriptors and endpoints (`SoftConnect`). A box with
  other descriptors while one is shown switches the same way.
- **Descriptors.** When the box is configured on the USB-A side (`ob_usb.cpp`), its device and configuration descriptors,
  plus every string descriptor they name, are read into RAM (`MirrorAttach`). They are served unchanged. None of the box's bytes are in the repo: the unit test's descriptor is made up (`tests/test_mirror_desc.cpp`).
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
  and `usbmidi=os:` still decides whether OS upgrades pass. See "What the filters cover" below.
- **Class and vendor requests.** Each one is forwarded to the box (`CObUsb::MirrorControl`). EP0 NAKs until the box
  answers, then returns the answer, or a STALL if the box stalls or is gone. One request is in flight at a time.
- **The box going away.** Its URBs in flight never complete, so when it goes (`MirrorBoxGone`) a pipe OUT packet the
  computer sent is dropped and the endpoint takes the next one, and a request forwarded to the box is STALLed. A box
  that comes back inside `mirrorhold` (a reboot after an OS update) starts from a clean state. A completion that arrives
  for an earlier attach is ignored (each attach has its own generation).
- **A halted pipe.** A failed transfer halts the endpoint on the xHC and on the box. `CObUsb::MirrorPipePoll` (core 0's
  loop) sends the box CLEAR_FEATURE(ENDPOINT_HALT), resets the host endpoint, and resubmits an IN pipe: every 100 ms for
  the first five tries, then once a second. The mirror line counts these (`reset`).
- **Interfaces of the audio stream.** `MirrorAttach` never sends SET_INTERFACE to them, so the Pi's own stream stays at
  alt 3. A pipe on such an interface is relayed only if it is at alt 3.
- **Overbridge's per-block bytes.** Bytes 4-31 of each OUT block (byte 1 = the first non-zero one's index, or 0xFF)
  carry data from the Engine, such as its latency-check marker; the box answers the marker with a pulse on IN. A
  computer block with any of them set has them copied into the Pi's own OUT block a fixed distance away
  (`mirrormargin=` blocks, 8, past where the Pi's OUT stood at the stream's first block), so the Engine measures the
  same latency on every try. The mirror line counts them (extra, lost, late).
- **The name Overbridge shows.** For an Elektron box (vendor 1935), the answer to vendor request 1 (IN, the 32-byte device
  name Overbridge shows in its Devices tab and plugin picker) gets a suffix, " (SC)" by default, so the unit reads
  "Analog Rytm MKII (SC)". A name the computer sets (OUT request 1) reaches the box without the suffix. Overbridge
  keys the unit on its PID and the unit ID of request 0, which pass unchanged, so plugins stay paired with the unit
  whether it is direct or through the Sidecar [I, read from Overbridge 2.26.10]. `mirrorname=off` turns it off;
  `mirrorname=TEXT` sets the suffix (an underscore for a space, a space put in front). The USB product string is left
  as the box's, since the computer names the MIDI and audio ports from it.

## What the filters cover

`usbmidi=os:` (`usbmidi_os.h`) filters MIDI only. In mirror mode two more paths reach the box, and both are relayed
unfiltered:

- the class and vendor requests on EP0 (Overbridge's name, unit ID, version and stream requests among them);
- the async channel (the vendor-class pipes).

So `usbmidi=os:off` does not stop a tool that updates the box over a vendor request or a pipe. The OS updates tested
through the mirror (the transfer tool and a SysEx flasher) went over USB MIDI: the mirror line's MIDI counters carried
the whole update to the box [hw]. With `usb=mirror` and any `usbmidi=os:` other than off, the engine logs a warning at
start.

## Circle change

`pi/circle/circle-gadget-mirror.patch` (applied by `pi/engine/build.sh` after `circle-gadget-reset.patch`; it defines
`CIRCLE_GADGET_MIRROR`) makes two changes, and `circle-gadget-softconnect.patch` adds a third:

1. **IN and OUT endpoints can share a number** (`0x83` + `0x03`). Circle kept one endpoint object per number. An OUT endpoint now goes to a second table when an IN endpoint already has its number. A gadget without shared numbers (the default Sidecar) never uses that table.
2. **A class or vendor request can be answered later.** The new hook is `OnControlRequest() -> ControlDeferred`, completed by `CompleteControl()`, with `OnControlAborted()` called when the host moves on. The default returns `ControlNotHandled`, so other gadgets take the old path unchanged.
3. **A replug without a cable.** `SoftDisconnect()` sets the controller's soft-disconnect bit and deactivates the endpoints. `SoftConnect()` sets the core up again, has the gadget remake its endpoints from its current descriptors, and clears the bit.

## Known limits

- **Box gone, inside mirrorhold.** The box is still shown and its requests are STALLed until the switch.
- **A switch mid-update.** An OS update whose box takes longer than `mirrorhold` to come back sees the Sidecar device meanwhile.
- **IRQs during a switch.** SoftConnect's core reset (about 25 ms) runs at task level with the gadget's interrupts off; the Overbridge stream on USB-A keeps running [I].
- **No forwarding of the computer's alt or interface choices.** Alternate settings stay on the gadget side, so a vendor request that only works at another box alternate is not handled. Requests that change the box's stream state also reach the box and may disturb the Pi's own stream.
- **One control request at a time.** A second one while the first is in flight is STALLed.
- **Which box.** The one the Pi's Overbridge driver claims. A box whose profile streams only after its Sidecar mod's hello (the Analog Four MKII) is not claimed without it, so a stock one is not mirrored.

## Tested

On a Pi 4 with an Analog Rytm MKII on OS 1.74, the computer on Windows [hw]:

- the default mode is unchanged (the Sidecar device, 12 in / 12 out plus MIDI, no underruns);
- the computer sees 1935:0B48 with the box maker's driver;
- Overbridge passes its latency check and the plugin syncs both ways; over 30 minutes no per-block bytes were lost or
  late, with no control stalls;
- per-track audio IN, and OUT to the Rytm;
- unplugging brings the Sidecar device back after `mirrorhold`; replugging brings the Rytm back in about 0.6 s, its name
  with " (SC)" and the plugin still paired;
- the transfer tool browses, elkflash `info` works through the mirrored MIDI port, and OS updates went through with
  `mirrorhold=60`, from the transfer tool and from a SysEx flasher.

In the emulator (rytm-emu over TCP): the default mode unchanged, and with `usb=mirror` and no box the Sidecar device
stays up.

The review changes of 2026-11-05 on the same unit [hw]:

- with the plugin streaming and the transfer tool browsing, the Rytm unplugged and replugged about 2 s later: the async
  IN pipe halted and was cleared and resumed, a request that came while the box was gone was STALLed, the computer never
  saw the Sidecar device, and Overbridge and the transfer tool carried on;
- unplugged past `mirrorhold`: every request STALLed while the box was away, the Sidecar device back after 60 s, the
  Rytm back with " (SC)" on replug;
- an OS update (stock 1.74) with elkflash over the mirrored MIDI port: 0 resends, the box back 0.3 s after its reboot
  dropped it, inside `mirrorhold`.

The Pi log's `mirror` line counts every path.
