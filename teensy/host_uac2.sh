#!/bin/bash
# teensy/host_uac2.sh [SECS]: the host build (the Rytm on the USB host port) that is also "Sidecar | Expander (Mini)", 20 in / 12 out
# USB Audio Class 2 + USB MIDI device for the computer on the micro-USB port (T3/T4, docs/superpowers/specs/
# 2026-10-16-overbridge-passthrough-spike.md sections 8-9): IN 1..12 the Rytm's Overbridge channels (MAIN L/R,
# BD SD RS BT LT MT CH CY, AUDIO IN L/R), IN 13..20 the Teensy's 8 voices (same track order), OUT 1..12 added to the
# Rytm's Overbridge inputs (MAIN L/R, MAIN FX L/R, the 8 tracks' inputs); MIDI both ways (usb_host.h). T3's 12/12
# without MIDI: HOST_UAC2_FLAGS="-DUAC2_CH_IN=12 -DUAC2_NO_MIDI" (later -D wins). Extra flags after the defaults: HOST_UAC2_FLAGS="-DRYTM_OUT_MAIN=0" teensy/host_uac2.sh.
# Boot defaults: out=tracks and MIDI notes off (the mod build plays each voice on its track, through the Rytm's
# filters and FX); ':out' and ':notes' as last set are kept in the emulated EEPROM with ':usbmode'. A stock Rytm
# (MIDI notes play the voices, the voices on MAIN): ':out=main', ':notes=1' once, or build with
# HOST_UAC2_FLAGS="-DRYTM_OUT_MAIN=1 -DRYTM_NOTES=1" (defaults only: the kept settings win).
# Back to the plain host build (Serial only; the stock-Rytm flags shown):
#   RMP_EXTRA_FLAGS="-DRYTM_USB_HOST=1 -DRYTM_NOTES=1 -DRYTM_OUT_MAIN=1" teensy/run_sketch.sh voiceloop
# RAM: RAM2 is nearly full in the host build, so the UAC2 rings, its DMA buffers (sized for high speed only) and USB
# MIDI's sit in RAM1 (DTCM: the USB DMA reaches it); feedback measured over 8 s (the Rytm's clock arrives in 7-14 frame steps).
D=$(cd "$(dirname "$0")" && pwd)
RMP_CORE=uac2 RMP_EXTRA_FLAGS="-DRYTM_USB_HOST=1 -DUAC2_CH_IN=20 -DUAC2_CH_OUT=12 \
-DUAC2_FB_WINDOW=65536 -DUAC2_HS_ONLY=1 -DUAC2_RING_FRAMES=256 -DUAC2_RING_ATTR= -DUAC2_BUF_ATTR= -DUSB_MIDI_BUF_ATTR= \
${HOST_UAC2_FLAGS}" \
  exec "$D/run_sketch.sh" voiceloop "${1:-30}"
