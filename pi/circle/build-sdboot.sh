#!/bin/bash
# Build sdboot.img: Circle's serial bootloader (tools/bootloader, 921600 baud) with pi/circle/sdboot.patch, which also
# boots engine.img from the SD card when no flasher answers within 1 s (docs/pi-engine.md, "Boot from the SD card").
# The Circle checkout stays clean: the build runs on a copy. Output: out/sdboot.img (and out/sdboot.lst).
set -euo pipefail
cd "$(dirname "$0")"
BUILD=${CIRCLE_BUILD:-$PWD/build}
mkdir -p out
docker run --rm -v "$PWD":/w -v "$BUILD/circle/tools/bootloader":/src:ro -w /tmp rytm-circle bash -euo pipefail -c '
  cp -r /src b && cd b && rm -f *.o *.elf *.lst *.img
  patch -s -p1 < /w/sdboot.patch
  make -s FLASHBAUD=921600 PREFIX64=aarch64-none-elf- kernel8-rpi4.img 2>&1 | grep -v "RWX permissions" || true
  cp kernel8-rpi4.img /w/out/sdboot.img && cp bootloader07_64_rpi4.lst /w/out/sdboot.lst
'
ls -l out/sdboot.img
