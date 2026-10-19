#!/bin/bash
# DSP SAMPLE's playback modes and the sample RAM's allocator (sim/sampler/
# sample_modes.h, sample_ram.h; docs/psram.md) as an Arduino library in
# libraries/Sampler, the same code the simulator runs. Git-ignored; rerun freely.
set -e
D=$(cd "$(dirname "$0")" && pwd)
L=$D/libraries/Sampler
rm -rf "$L"; mkdir -p "$L/src"
cp "$D/../sim/sampler/sample_modes.h" "$D/../sim/sampler/sample_ram.h" "$L/src/"
cat > "$L/src/Sampler.h" <<'H'
// Arduino finds a library by a header at the top of src/: include this first.
#pragma once
#include "sample_modes.h"
#include "sample_ram.h"
H
cat > "$L/library.properties" <<'P'
name=Sampler
version=0.0.0
author=rytm-teensy
maintainer=rytm-teensy
sentence=DSP SAMPLE's playback modes and the sample RAM (PSRAM) allocator
paragraph=
category=Signal Input/Output
url=
architectures=*
P
echo "set up $L"
