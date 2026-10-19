#!/bin/bash
# Build the DAISYSP voice (daisy_engine.h on DaisySP, Electrosmith, MIT) as a
# native library for the Teensy simulator. DaisySP at a pinned commit in vendor/
# (git-ignored); only its physical-modelling sources are compiled, none of its
# own build scripts run. Nothing from it is committed here.
set -e
D=$(cd "$(dirname "$0")" && pwd); V=$D/vendor/DaisySP
if [ ! -d "$V/Source" ]; then
  mkdir -p "$D/vendor"
  git clone --quiet https://github.com/electro-smith/DaisySP.git "$V"
fi
git -C "$V" checkout --quiet 2c72eaf9eac5fc0dca1919d65d606da907832618
# daisy_engine.h's own resonator and string (BlockResonator, BlockString) use
# DaisySP's filters, delay line and DC blocker only
SRC="$V/Source/Filters/svf.cpp $V/Source/Utility/dcblock.cpp"
mkdir -p "$D/build"
clang++ -std=c++14 -O2 -fPIC -shared -I"$V/Source" -I"$V/Source/Utility" -I"$V/Source/Noise" -I"$V/Source/Filters" -I"$V/Source/PhysicalModeling" -I"$D" -I"$D/../sampler" -o "$D/build/libdaisy.dylib" "$D/daisy_c.cc" $SRC
echo "built $D/build/libdaisy.dylib"
