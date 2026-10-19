# sidecar-1019: internal notes

Base line for the Monday (Teensy) and Tuesday (Pi) tester releases. One branch builds both the Pi kernel and the
Teensy 4.1 host-build firmware. Not merged to main.

## What's in it

From `slot-resolve` (c7710b0), then merged in order:

- **slot-resolve**: pi-heap (heap buffers, `_end` about 3.3 MB), pi-wifi, teensy-webui-plugins, slot resolution
  (a user machine slot resolves from the project's sounds; 0x25 slot prints from sfx1019).
- **pi-record** (9f6f162): the Pi multitrack recorder (card, Record tab on sidecar.local, REC on the display),
  the web connection cap, an optional per-FX return output from `SendFx::Process` (the recorder's FX1/FX2 tracks).
- **sfx-enum-knobs** (87c02e8, 92fe867): the 0x24 per-zone step bit (STUTTER ENG/LEN step one zone per detent).
- **teensy-1018** (7f5e079): the Teensy send FX CHORUS, COMB, CRUSH (16), RINGMOD (17) in the host build,
  block-batched (about 4-5 % CPU). CRUSH and RINGMOD also play on the Pi.
- release-ui-tagline and pi-wifi were already in.

## Pairs with

rytm-mods `build/sidecar-1019/`: **sfx1019** (send FX + enum knobs + 0x25 slot prints) and **rui1019** (remote UI).
sfx1018 still works but takes the lazy path for user machine slots (no 0x25).

## Hardware-verified vs emulator-only

Hardware-verified (2026-10-17 and earlier):
- send FX on the Pi, including labels, PRE/POST and STUTTER
- the Pi recorder, 5-minute take
- user machines load
- web tabs, folder upload
- the touch fix
- Wi-Fi join
- Teensy send FX live, with CHORUS and COMB

Emulator-only (not yet on a real Rytm):
- enum knobs (the 0x24 step bit on the Rytm side)
- slot menu fixes
- slot resolution
- sfx1019's 0x25 slot prints

Not yet re-verified on hardware: this merged line as a whole (the per-FX returns now go through the Teensy's
block-batched `Process`), CRUSH and RINGMOD on either device.

## Known open items (emulator)

`sim/sendfx_e2e.py all` against sfx1019: 30 pass, 3 fail. The core's send FX code here is the same as teensy-1018's,
so these come from teensy-1018, not from the merge. The 0x21 list is now
OFF CHORUS COMB CRUSH RINGMOD RINGS CLOUDSEED STUTTER and goes out 8 names to a message, so UFX MYVERB ends up in a
second message. The list is also no longer in id order, so one step up from the unlisted id 7 lands on 17
(RINGMOD), not 8. The test still expects one message and id order. So does the TYPE-fade check, which expects the
comb after the step. Decide whether to sort the list by id or update the test before the Rytm side relies on stepping
from an unlisted id.
