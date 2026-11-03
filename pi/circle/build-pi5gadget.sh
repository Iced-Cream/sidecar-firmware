#!/bin/bash
# pi/circle/build-pi5gadget.sh - the Pi 5 USB-C bring-up kernel, in Docker (docs/pi5.md "Bring-up kernel"):
#   pi/circle/pi5gadget -> pi/circle/out/pi5gadget/kernel_2712.img
# Its Circle tree, build/circle-pi5gadget: a copy of build/circle (run build.sh first), RASPPI=5, single core,
# circle-gadget.patch + circle-gadget-wake.patch + circle-pi5-gadget.patch + circle-gadget-reset.patch, USB_GADGET_DEBUG (every event logged), a 32 KB serial buffer (a full one drops text). Made again when a patch changes.
# Not the engine: the engine's test kernels for tryboots (testsecs=, /usbtest.log, testcard=1) are
# PI5_GADGET=1 PI5_TESTMODE=1 RASPPI=5 pi/engine/build.sh; PI5_GADGET=1 alone (the Pi 5 default) is the release build.
# CIRCLE_BUILD: the build dir, as for pi/engine/build.sh (a worktree points it at the main checkout's build/).
set -euo pipefail
cd "$(dirname "$0")"
REPO=$(git rev-parse --show-toplevel)
BUILD=${CIRCLE_BUILD:-$PWD/build}
docker image inspect rytm-circle >/dev/null 2>&1 || docker build -t rytm-circle "$REPO/pi/circle"
docker run --rm -v "$REPO":/repo -v "$BUILD":/repo/pi/circle/build -w /repo/pi/circle rytm-circle bash -euo pipefail -c '
T=/repo/pi/circle/build/circle-pi5gadget
J=$(nproc)
SUM=$(cat circle-gadget.patch circle-gadget-wake.patch circle-pi5-gadget.patch circle-gadget-reset.patch gadget_wake.h gadget_flush.h | md5sum)
if [ "$(cat $T/.pi5gadget-sum 2>/dev/null)" != "$SUM" ]; then
  [ -d build/circle ] || { echo "run pi/circle/build.sh first (the Circle checkout)"; exit 1; }
  rm -rf $T && cp -a build/circle $T
  cd $T
  ./makeall --nosample clean >/dev/null 2>&1 || true
  patch -p1 < /repo/pi/circle/circle-gadget.patch
  cp /repo/pi/circle/gadget_wake.h include/circle/usb/gadget/gadgetwake.h
  patch -p1 < /repo/pi/circle/circle-gadget-wake.patch
  patch -p1 < /repo/pi/circle/circle-pi5-gadget.patch
  cp /repo/pi/circle/gadget_flush.h include/circle/usb/gadget/gadgetflush.h
  patch -p1 < /repo/pi/circle/circle-gadget-reset.patch
  ./configure -f -r 5 -p aarch64-none-elf- -d USB_GADGET_VENDOR_ID=0x1209 -d USB_GADGET_DEVICE_ID_BASE=0x0001 -d USB_GADGET_DEBUG -d SERIAL_BUF_SIZE=32768
  ./makeall --nosample -j$J >/dev/null
  echo "$SUM" > .pi5gadget-sum
  cd /repo/pi/circle
fi
make -C pi5gadget CIRCLEHOME=$T clean >/dev/null
make -C pi5gadget CIRCLEHOME=$T -j$J
mkdir -p out/pi5gadget
cp pi5gadget/kernel_2712.img out/pi5gadget/
'
ls -l out/pi5gadget/kernel_2712.img
