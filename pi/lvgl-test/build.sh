#!/bin/bash
# pi/lvgl-test/build.sh - the display test kernel, in Docker (pi/circle's rytm-circle image) -> kernel8-rpi4.img.
# Built against the engine's Circle tree (circle-engine: pi/engine/build.sh first) with LVGL added to it
# (pi/ui/fetch-lvgl.sh, pi/ui/lvgl-in-tree.sh). CIRCLE_BUILD: as flash.sh (a worktree points it at the main
# checkout's pi/circle/build).
set -euo pipefail
cd "$(dirname "$0")"
REPO=$(git rev-parse --show-toplevel)
BUILD=${CIRCLE_BUILD:-$REPO/pi/circle/build}
[ -f "$BUILD/circle-engine/.engine-ready" ] || { echo "run pi/engine/build.sh first (circle-engine)"; exit 1; }
../ui/fetch-lvgl.sh "$BUILD"
docker run --rm -v "$REPO":/repo -v "$BUILD":/repo/pi/circle/build -w /repo/pi/lvgl-test rytm-circle bash -euo pipefail -c '
T=/repo/pi/circle/build/circle-engine
../ui/lvgl-in-tree.sh $T
make CIRCLEHOME=$T -j$(nproc)
'
ls -l kernel8-rpi4.img
