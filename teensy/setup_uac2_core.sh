#!/bin/bash
# teensy/setup_uac2_core.sh: makes teensy/cores-uac2/teensy4, a patched copy of the installed Teensyduino
# 1.62.0 core with USB type USB_SERIAL_UAC2 (Serial + USB Audio Class 2: teensy/uac2/). The installed core
# is never edited. The copy is git-ignored (PJRC's files); only the patch and our own files are committed.
# Remade when missing or when the patch changed; usb_uac2.c/.h are copied over when they differ.
# run_sketch.sh calls this when RMP_CORE=uac2.
set -eo pipefail
D=$(cd "$(dirname "$0")" && pwd)
SRC=~/Library/Arduino15/packages/teensy/hardware/avr/1.62.0/cores/teensy4
PATCH="$D/uac2/teensy4-1.62.0-uac2.patch"
DST="$D/cores-uac2/teensy4"
[ -d "$SRC" ] || { echo "setup_uac2_core.sh: Teensyduino 1.62.0 core not found at $SRC (the patch is for 1.62.0)"; exit 1; }
SUM=$(shasum "$PATCH" | cut -c1-12)
if [ "$(cat "$D/cores-uac2/.stamp" 2>/dev/null)" != "$SUM" ] || [ ! -d "$DST" ]; then
  rm -rf "$D/cores-uac2"; mkdir -p "$D/cores-uac2"
  cp -R "$SRC" "$DST"
  (cd "$DST" && patch -s -p1 < "$PATCH")
  echo "$SUM" > "$D/cores-uac2/.stamp"
  echo "setup_uac2_core.sh: made $DST"
fi
for f in usb_uac2.c usb_uac2.h; do cmp -s "$D/uac2/$f" "$DST/$f" || cp "$D/uac2/$f" "$DST/$f"; done
echo "$DST"
