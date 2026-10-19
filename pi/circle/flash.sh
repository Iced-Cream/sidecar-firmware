#!/bin/bash
# Upload a spike kernel to the Pi 4 over the serial cable and log its output, no SD card swaps.
# One-time setup:
#   1. Copy out/sd-boot/* (Circle's serial bootloader at 921600 baud, built by
#      `make -C build/circle/boot all64` with FLASHBAUD = 921600 in build/circle/Config.mk) to the FAT32 card.
#   2. Serial adapter: GND to pin 6, RX to pin 8 (GPIO14), TX to pin 10 (GPIO15), VCC not connected.
#   3. `npm install` in build/circle/tools/flashy.
# Then:  ./flash.sh [spike2] [SECS]   builds DIR's .hex, reboots a running spike kernel by its magic
# (RYTM-REBOOT), flashes, and monitors for SECS seconds (default 30), logging to out/<dir>-<time>.log.
# spike2 logs at 921600 baud (since the audio capture, capture.py; spike1: 115200). The reboot magic is
# sent at --userbaud, so it must match the running kernel: USERBAUD=115200 ./flash.sh replaces an
# older 115200 build.
# CIRCLE_TREE=circle-engine ./flash.sh ../engine [SECS]: the Pi engine (pi/engine/build.sh first).
# SECS counts from the flasher's start, upload included (a ~600 KB kernel's .hex takes about 8 s at 921600 baud).
# FLASH_SECS_AFTER_GO=1: SECS counts from the "go" instead (flashy's "Monitoring" line), so the kernel itself runs
# about SECS seconds; the wait for that line is capped at FLASH_GO_WAIT seconds (default 120). The last line of the
# output is "log: <path>" in both modes; flashy's own lines may follow it (they pass through tee).
set -euo pipefail
cd "$(dirname "$0")"
DIR=${1:-spike2}; SECS=${2:-30}; USERBAUD=${USERBAUD:-921600}
REPO=$(git rev-parse --show-toplevel)
BUILD=${CIRCLE_BUILD:-$PWD/build}          # a worktree may point this at the main checkout's build/
PORT=$(ls /dev/cu.usbserial-* 2>/dev/null | head -1)
[ -n "$PORT" ] || { echo "no serial adapter (/dev/cu.usbserial-*)"; exit 1; }
TREE=${CIRCLE_TREE:-circle}                # pi/engine uses circle-engine (pi/engine/build.sh)
docker run --rm -v "$REPO":/repo -v "$BUILD":/repo/pi/circle/build -w /repo/pi/circle rytm-circle \
  make -C "$DIR" CIRCLEHOME=/repo/pi/circle/build/$TREE kernel8-rpi4.hex >/dev/null
mkdir -p out
LOG=out/$(basename "$DIR")-$(date +%Y%m%d-%H%M%S).log
node "$BUILD/circle/tools/flashy/flashy.js" "$PORT" "$DIR/kernel8-rpi4.hex" \
  --flashbaud:921600 --userbaud:$USERBAUD --reboot:RYTM-REBOOT --monitor > >(tee "$LOG") 2>&1 &
PID=$!
if [ "${FLASH_SECS_AFTER_GO:-0}" = 1 ]; then
  T0=$SECONDS
  until grep -q '^Monitoring' "$LOG" 2>/dev/null || ! kill -0 $PID 2>/dev/null ||
        [ $((SECONDS - T0)) -ge "${FLASH_GO_WAIT:-120}" ]; do sleep 0.2; done
  if grep -q '^Monitoring' "$LOG" 2>/dev/null; then echo "upload: go after $((SECONDS - T0)) s"
  elif ! kill -0 $PID 2>/dev/null; then echo "upload: flashy exited before the go"
  else echo "upload: timed out after $((SECONDS - T0)) s waiting for the go (FLASH_GO_WAIT)"; fi
fi
sleep "$SECS"
kill $PID 2>/dev/null || true
echo "log: $LOG"
