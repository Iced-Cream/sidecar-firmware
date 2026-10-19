#!/bin/bash
# sim/rmp/build.sh - the harness library for sim/rmpvoice.py: rmp_native.cpp with the Teensy's portable
# pool reader, preset state, manifest reader and loader (teensy/libraries/RytmPlugins/src)
set -e; cd "$(dirname "$0")"
L=../../teensy/libraries/RytmPlugins/src
c++ -std=c++17 -O2 -Wall -shared -fPIC -I../../sdk -I"$L" -o librmp_native.dylib rmp_native.cpp \
  "$L/rmp_pool.cpp" "$L/rmp_presets.cpp" "$L/rmp_manifest.cpp" "$L/rmp_loader.cpp"
