# Circle's xHCI set up as Linux does for the VL805 (circle-xhci-vl805.patch)

Status: built, not yet run on a unit (2026-11-17).

## Why
With both boxes on the Pi 4 (Rytm on USB-C, A4 on the Teensy relay, or the default route), once about 6 interrupt
endpoints at bInterval 1 are busy on the VL805, one of them is served only k = 0..7 times per ms. A short box OUT runs in
steps of 984.4/s = 1000 x 63/64: one transfer per service, with one service in 64 landing on the link TRB of Circle's
64-entry transfer ring. Linux runs the VL805 with these endpoints and differs from Circle in three places on that path
(offline comparison, Circle Step51.1.1 vs raspberrypi/linux rpi-6.6.y drivers/usb/host):

| | Circle Step51.1.1 | Linux (VIA 0x3483 quirks) | this patch |
|---|---|---|---|
| Interrupt EP Average TRB Length | 16 ("best guess") | Max ESIT Payload (368 audio, 64 pipe) | wMaxPacketSize (= Circle's Max ESIT Payload) |
| Transfer ring | 64 TRBs, link every 63 | 256-TRB segments | 256 TRBs, link every 255 |
| Ring memory | exact size (2 KB pool block) | segment pool at 2x size, 2x aligned (XHCI_ZHAOXIN_TRB_FETCH) | 2x size, 8 KB block aligned to 8 KB |
| Set TR Dequeue on a link TRB | not possible (enqueue index never rests on it) | skipped past (XHCI_AVOID_DQ_ON_LINK) | checked: refused with a warning |
| Context DCS read back | never (ring's own cycle state) | TRB's cycle bit used (XHCI_EP_CTX_BROKEN_DCS) | unchanged: Circle never reads it |

XHCI_VLI_SS_BULK_OUT_BUG (SuperSpeed bulk) and XHCI_VLI_HUB_TT_QUIRK (FS/LS behind a TT) do not apply to these
high-speed devices.

## Memory
The xHCI shared memory pool (3 MB) only reused 2 KB blocks; anything larger was lost when freed. The patch adds a second
kept class (8 KB, 8 KB aligned) for the rings, so a device leaving (Rytm restart, Teensy flash) returns its rings. Each ring
costs 16 KB of the pool with the alignment: ~30 rings for two boxes, the relay and the hub is ~0.5 MB. Host check of the
allocator (40 rings, 1000 free/allocate rounds): no growth.

## What a unit run shows
- Boot log: `xhcidev: circle-xhci-vl805: rings 256 TRB (x2 alloc), interrupt avg TRB = MPS`.
- If the link TRB was the cause: no short box OUT with 6 every-microframe endpoints (default route, both async pipes at
  their own interval). If short OUT remains, its step tells which part mattered: 996.1/s (= 1000 x 255/256) means the link TRB
  still costs a service but less often; 984.4/s would mean the old ring is still in use (check the boot line).
