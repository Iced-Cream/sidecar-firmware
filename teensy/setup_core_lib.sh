#!/bin/bash
# The engine core (core/: portable C++17) with the Teensy's port (teensy/platform/) as an Arduino library in
# libraries/RytmCore. Git-ignored like the other generated libraries; run_sketch.sh runs it before every build.
# Only the core's library sources go in: not its Mac port (core/host/), the card-store user machines (core/user/: the
# Pi and the Mac; the Teensy has its own plugin glue), tests, build output or scripts.
set -e
D=$(cd "$(dirname "$0")" && pwd)
C=$(cd "$D/../core" && pwd)
L=$D/libraries/RytmCore
"$D/../tools/gen_version.sh"   # core/version.h (VERSION, the commit): copied with the core
rm -rf "$L"; mkdir -p "$L/src"
(cd "$C" && find . \( -path ./host -o -path ./tests -o -path ./build -o -path ./user \) -prune -o \( -name '*.h' -o -name '*.cpp' \) -print) |
  while read -r f; do mkdir -p "$L/src/$(dirname "$f")"; cp "$C/$f" "$L/src/$f"; done
cp "$D/platform/core_port.h" "$D/platform/platform_teensy.cpp" "$L/src/"
cat > "$L/src/RytmCore.h" <<'H'
// Arduino finds a library by a header at the top of src/: include this first.
#pragma once
#include "core.h"
H
cat > "$L/library.properties" <<'P'
name=RytmCore
version=0.1.0
author=rytm-teensy
maintainer=rytm-teensy
sentence=The engine core (core/) with the Teensy's port (teensy/platform/)
paragraph=
category=Signal Input/Output
url=
architectures=*
P
echo "set up $L ($(find "$L/src" -name '*.cpp' | wc -l | tr -d ' ') sources)"
