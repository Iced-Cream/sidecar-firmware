#!/bin/bash
# Build the JUNO voice (juno_voice.h) as a native library for the simulator.
set -e
D=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$D/build"
clang++ -std=c++14 -O2 -fPIC -shared -I"$D" -o "$D/build/libjuno.dylib" "$D/juno_c.cc"
echo "built $D/build/libjuno.dylib"
