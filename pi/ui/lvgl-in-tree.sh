#!/bin/bash
# pi/ui/lvgl-in-tree.sh TREE - in the Docker build: LVGL (BUILD/lvgl-src, fetch-lvgl.sh) into TREE/addon/lvgl/lvgl and
# TREE/addon/lvgl/liblvgl.a built, once per LVGL commit. Nothing else in the tree changes.
set -euo pipefail
T=$1; S=$(dirname "$T")/lvgl-src
if [ "$(cat "$T/addon/lvgl/lvgl/.commit" 2>/dev/null)" != "$(cat "$S/.commit")" ]; then
  rm -rf "$T/addon/lvgl/lvgl" && cp -a "$S" "$T/addon/lvgl/lvgl" && rm -f "$T/addon/lvgl/liblvgl.a"
fi
[ -f "$T/addon/lvgl/liblvgl.a" ] || make -C "$T/addon/lvgl" -j"$(nproc)" >/dev/null
