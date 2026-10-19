#!/bin/bash
# pi/ui/fetch-lvgl.sh BUILD - LVGL (MIT) into BUILD/lvgl-src once, at the commit Circle 51.1.1's addon/lvgl submodule
# pins (LVGL 9.4.0). pi/engine/build.sh and pi/lvgl-test/build.sh copy it into the circle-engine tree's addon/lvgl/lvgl
# and build liblvgl.a there (Circle's addon Makefile and lv_conf.h).
set -euo pipefail
LVGL_COMMIT=c016f72d4c125098287be5e83c0f1abed4706ee5
D=$1/lvgl-src
[ "$(cat "$D/.commit" 2>/dev/null)" = $LVGL_COMMIT ] && exit 0
rm -rf "$D" && git init -q "$D"
git -C "$D" fetch -q --depth 1 https://github.com/lvgl/lvgl.git $LVGL_COMMIT
git -C "$D" checkout -q FETCH_HEAD && rm -rf "$D/.git"
echo $LVGL_COMMIT > "$D/.commit"
