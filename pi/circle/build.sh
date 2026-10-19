#!/bin/bash
# Build Circle (RASPPI=4, AArch64) and the spike kernels inside Docker.
#   ./build.sh            everything: image, Circle (real + QEMU variant), samples, spike1, spike2
#   ./build.sh spike      only spike1/spike2 (Circle already built)
# Outputs in out/: spike1-rpi4.img (real Pi 4), spike1-qemu.img (raspi4b under QEMU),
#   spike2-rpi4.img (USB stub, real Pi 4 only), sample-01-gpiosimple.img, sample-34-sounddevices.img,
#   sample-29-miniorgan.img.  Circle trees: build/circle (real) and build/circle-qemu.
set -euo pipefail
cd "$(dirname "$0")"
CIRCLE_TAG=${CIRCLE_TAG:-Step51.1.1}
IMAGE=rytm-circle
REPO=$(git rev-parse --show-toplevel)

docker image inspect $IMAGE >/dev/null 2>&1 || docker build -t $IMAGE .

docker run --rm -v "$REPO":/repo -w /repo/pi/circle -e CIRCLE_TAG=$CIRCLE_TAG -e MODE="${1:-all}" $IMAGE bash -euo pipefail -c '
PFX=aarch64-none-elf-
J=$(nproc)
W=/repo/pi/circle
mkdir -p build out
if [ "$MODE" = all ]; then
  [ -d build/circle ] || git clone -q --depth 1 --branch $CIRCLE_TAG https://github.com/rsta2/circle.git build/circle
  cd build/circle
  ./configure -f -r 4 -p $PFX
  ./makeall --nosample -j$J
  for s in 01-gpiosimple 34-sounddevices 29-miniorgan; do
    make -C sample/$s -j$J; cp sample/$s/kernel8-rpi4.img $W/out/sample-$s.img
  done
  cd $W
  # QEMU tree (NO_SDHOST etc.): a copy, so the real one stays clean
  rm -rf build/circle-qemu && cp -a build/circle build/circle-qemu
  cd build/circle-qemu
  ./makeall --nosample clean >/dev/null 2>&1 || true
  ./configure -f -r 4 -p $PFX --qemu
  ./makeall --nosample -j$J
  cd $W
fi
# USB patches for spike2 (idempotent), then rebuild lib/usb in the real tree
cd build/circle
if ! grep -q RytmObDeviceCreate lib/usb/usbdevicefactory.cpp; then
  patch -p1 < $W/spike2/circle-usbdevicefactory.patch
fi
# up to 16 URBs per xHCI endpoint, endpoint state and a transfer-event hook (idempotent)
if ! grep -q XHCI_EP_MAX_URBS include/circle/usb/xhciendpoint.h; then
  patch -p1 < $W/spike2/circle-xhci-urbqueue.patch
fi
make -C lib/usb -j$J
cd $W
for k in spike1:circle spike1:circle-qemu spike2:circle; do
  d=${k%%:*}; t=${k##*:}
  make -C $d CIRCLEHOME=$W/build/$t clean
  make -C $d CIRCLEHOME=$W/build/$t -j$J
  case $t in circle-qemu) cp $d/kernel8-rpi4.img out/$d-qemu.img;; *) cp $d/kernel8-rpi4.img out/$d-rpi4.img;; esac
done
'
ls -l out/
