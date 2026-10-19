#!/bin/bash
# Build spike1 for a Raspberry Pi 3 (RASPPI=3, AArch64) and assemble an SD folder, out/sd3/.
# Needs the Docker image and build/circle from ./build.sh (the Pi 4 build) first.
#   ./build-rpi3.sh     then copy out/sd3/* to a FAT32 (MBR) card
# The card's config.txt also has a [pi4] section: add out/sd/'s Pi 4 files to the same card
# (armstub8-rpi4.bin, kernel8-rpi4.img, start4.elf, fixup4.dat, bcm2711-rpi-4-b.dtb) and it boots both.
set -euo pipefail
cd "$(dirname "$0")"
FW=0641c5bf25d185d5bf25d8dcafd1da6dc8fdbf68   # the firmware commit Circle 51.1.1's boot/Makefile pins
docker run --rm -v "$PWD":/w -w /w rytm-circle bash -euo pipefail -c '
PFX=aarch64-none-elf-; J=$(nproc)
rm -rf build/circle-rpi3 && cp -a build/circle build/circle-rpi3
cd build/circle-rpi3
./makeall --nosample clean >/dev/null 2>&1 || true
git checkout -q -- lib/usb/usbdevicefactory.cpp 2>/dev/null || true   # spike2 hook not needed here
./configure -f -r 3 -p $PFX >/dev/null
./makeall --nosample -j$J >/dev/null
make -C /w/spike1 CIRCLEHOME=/w/build/circle-rpi3 clean >/dev/null
make -C /w/spike1 CIRCLEHOME=/w/build/circle-rpi3 -j$J >/dev/null
'
mkdir -p build/fw3 out/sd3
B=https://github.com/raspberrypi/firmware/raw/$FW/boot
for f in bootcode.bin start.elf fixup.dat bcm2710-rpi-3-b.dtb bcm2710-rpi-3-b-plus.dtb LICENCE.broadcom; do
  [ -f build/fw3/$f ] || curl -sSfL -o build/fw3/$f $B/$f
done
cp build/fw3/* out/sd3/
cp spike1/kernel8.img out/sd3/
cat > out/sd3/config.txt <<'CFG'
arm_64bit=1
kernel_address=0x80000
initial_turbo=0
enable_uart=1

[pi3]
kernel=kernel8.img

[pi3+]
kernel=kernel8.img

[pi4]
armstub=armstub8-rpi4.bin
kernel=kernel8-rpi4.img
CFG
ls -l out/sd3/
