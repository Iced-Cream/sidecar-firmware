#!/bin/bash
# Make a ready-to-copy FAT32 SD folder (out/sd/) and a 64 MB FAT32 image (out/sd.img) for a Pi 4.
#   ./make-sd.sh [spike1|spike2|sample-34-sounddevices|sample-29-miniorgan|sample-01-gpiosimple]
# Needs ./build.sh run first. Downloads the Raspberry Pi firmware (pinned commit, same as Circle's boot/Makefile)
# into build/fw/ once, builds Circle's armstub8-rpi4.bin (Docker).
# Copy the contents of out/sd/ to a FAT32-formatted card, or write the image:  diskutil unmountDisk /dev/diskN &&
#   sudo dd if=out/sd.img of=/dev/rdiskN bs=1m   (the card then shows a 64 MB FAT32 volume).
# Serial: 115200 8N1 on GPIO14 (TXD, pin 8) / GPIO15 (RXD, pin 10), GND pin 6, 3.3 V adapter, no VCC.
set -euo pipefail
cd "$(dirname "$0")"
K=${1:-spike1}
case $K in
  spike1|spike2) SRC=out/$K-rpi4.img;;
  sample-*) SRC=out/$K.img;;
  *) SRC=$K;;
esac
[ -f "$SRC" ] || { echo "missing $SRC: run ./build.sh first"; exit 1; }

FW=$(grep -E '^FIRMWARE \?=' build/circle/boot/Makefile | awk '{print $3}')
BASE=https://github.com/raspberrypi/firmware/raw/$FW/boot
mkdir -p build/fw
for f in start4.elf fixup4.dat bcm2711-rpi-4-b.dtb LICENCE.broadcom; do
  [ -s build/fw/$f ] || { echo "downloading $f"; curl -fsSL -o build/fw/$f $BASE/$f; }
done

docker image inspect rytm-circle >/dev/null
docker run --rm -v "$(git rev-parse --show-toplevel)":/repo -w /repo/pi/circle rytm-circle bash -euo pipefail -c '
  make -C build/circle/boot/armstub armstub8-rpi4.bin
  cp build/circle/boot/armstub/armstub8-rpi4.bin build/fw/
'

rm -rf out/sd; mkdir -p out/sd
cp build/fw/{start4.elf,fixup4.dat,bcm2711-rpi-4-b.dtb,LICENCE.broadcom,armstub8-rpi4.bin} out/sd/
cp "$SRC" out/sd/kernel8-rpi4.img
cat > out/sd/config.txt <<'CFG'
arm_64bit=1
kernel_address=0x80000
initial_turbo=0
enable_uart=1

[pi4]
armstub=armstub8-rpi4.bin
kernel=kernel8-rpi4.img
CFG
echo "kernel: $SRC" > out/sd/KERNEL.txt

# a flat FAT32 image (mtools, no root needed)
docker run --rm -v "$(git rev-parse --show-toplevel)":/repo -w /repo/pi/circle rytm-circle bash -euo pipefail -c '
  rm -f out/sd.img; truncate -s 64M out/sd.img
  mkfs.fat -F 32 -n RYTMPI out/sd.img >/dev/null
  mcopy -i out/sd.img out/sd/* ::
  mdir -i out/sd.img ::
'
echo "SD folder: $PWD/out/sd   image: $PWD/out/sd.img"
