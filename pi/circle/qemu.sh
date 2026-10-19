#!/bin/bash
# Boot the QEMU build of spike1 on QEMU's raspi4b and show the serial console.
#   ./qemu.sh [seconds] [machine]     default 8 s, raspi4b (raspi3b needs a RASPPI=3 build, not provided)
set -u
cd "$(dirname "$0")"
T=${1:-8}; M=${2:-raspi4b}
IMG=out/spike1-qemu.img
[ -f $IMG ] || { echo "run ./build.sh first"; exit 1; }
# -kernel loads the AArch64 image at 0x80000 behind QEMU's built-in boot stub
qemu-system-aarch64 -M "$M" -kernel "$IMG" -display none -serial stdio -monitor none 2>&1 &
QP=$!
sleep "$T"; kill $QP 2>/dev/null; wait $QP 2>/dev/null
echo "[qemu stopped after ${T}s]"
