#!/bin/bash
# Build DSP SAMPLE's playback modes and the sample RAM's allocator
# (sample_modes.h, sample_ram.h; docs/psram.md) as a native library for the
# simulator (sim/samplemodes.py). No vendor code.
set -e
D=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$D/build"
clang++ -std=c++14 -O2 -Wall -Wextra -fPIC -shared -I"$D" -o "$D/build/libsampler.dylib" "$D/sampler_c.cc"
# without the read counts and the cache model: for timing (samplebench.py)
clang++ -std=c++14 -O2 -Wall -Wextra -fPIC -shared -DRYTM_NO_OPS -I"$D" -o "$D/build/libsampler_fast.dylib" "$D/sampler_c.cc"
echo "built $D/build/libsampler.dylib, libsampler_fast.dylib"
