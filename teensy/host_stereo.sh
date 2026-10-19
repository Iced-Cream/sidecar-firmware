#!/bin/bash
# teensy/host_stereo.sh [SECS]: the Sidecar Mini - the host build (the Rytm on the USB host port) that is also a stereo
# USB audio interface for the computer on the micro-USB port: Serial (the console, the web page) + USB Audio Class 2,
# 2 in / 2 out at 48 kHz, 24-bit, + USB MIDI (the Rytm's MIDI to the computer and back, usb_host.h). IN 1/2 = the Rytm's MAIN L/R as its Overbridge sends it; OUT 1/2 = added to
# the Rytm's MAIN input. The Rytm is the clock (the feedback and the IN packet sizes follow its Overbridge frames).
# ':usbmode=stereo|off' (kept across reboots; stereo before the first change). PJRC's vendor ID as every build.
# The 20 in / 12 out build: teensy/host_uac2.sh. RAM: both 128-frame rings (1 KB each) share the recorder's 32 KB
# ring in RAM1 (UAC2_RING_MEM=rec_pool, rec_mini.h: USB audio pauses - the device stays, IN silent - while the recorder
# is armed or records, without PSRAM); the UAC2 DMA buffers (800 B) in RAM1 too (DTCM: the USB DMA reaches it). USB MIDI's buffers small and in RAM2
# (2 x 512 B receive, 4 x 128 B transmit: short packets), Serial's transmit buffers 4 x 1 KB (PJRC's 4 x 2 KB).
# RYTM_DEBUG_TOOLS=0: no ':sendfxbench' / ':cap' (developers' tools) in this, the release build.
D=$(cd "$(dirname "$0")" && pwd)
RMP_CORE=uac2 RMP_EXTRA_FLAGS="-DRYTM_USB_HOST=1 -DUAC2_CH_IN=2 -DUAC2_CH_OUT=2 \
-DRYTM_DEBUG_TOOLS=0 -DUAC2_FB_WINDOW=65536 -DUAC2_HS_ONLY=1 -DUAC2_RING_FRAMES=128 -DUAC_IN_FRAMES=128 \
-DUAC2_RING_MEM=rec_pool -DUAC2_BUF_ATTR= -DUSB_SERIAL_TX_SIZE=1024 -DUSB_MIDI_TX_SIZE=128 -DUSB_MIDI_RX_NUM=2${HOST_STEREO_FLAGS:+ $HOST_STEREO_FLAGS}" \
  exec "$D/run_sketch.sh" voiceloop "${1:-30}"
