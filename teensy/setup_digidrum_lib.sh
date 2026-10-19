#!/bin/bash
# Copy digidrum's machines (soejrd/digidrum, MIT) and the DIGIDRUM voice
# (sim/digidrum/dd_voice.*) into libraries/Digidrum as an Arduino library, from
# the pinned checkout that sim/digidrum/build.sh sets up. Git-ignored; rerun
# freely.
set -e
D=$(cd "$(dirname "$0")" && pwd)
"$D/../sim/digidrum/build.sh" >/dev/null   # makes sure the vendor checkout exists
V=$(cd "$D/../sim/digidrum/vendor/digidrum" && pwd -P)
L=$D/libraries/Digidrum
rm -rf "$L"; mkdir -p "$L/src"
cp -R "$V/include" "$V/machines" "$V/dsp" "$L/src/"
# digidrum's build puts include/ on the path; Arduino only has src/.
for f in "$L"/src/dsp/*.c "$L"/src/machines/*.c; do
  sed -i '' 's|#include "dd_|#include "../include/dd_|' "$f"
done
cp "$D/../sim/digidrum/dd_voice.h" "$D/../sim/digidrum/dd_voice.c" "$L/src/"
cat > "$L/src/Digidrum.h" <<'H'
// Arduino finds a library by a header at the top of src/: include this first.
#pragma once
#include "dd_voice.h"
H
cat > "$L/library.properties" <<'P'
name=Digidrum
version=0.0.0
author=soejrd (digidrum, MIT); rytm-teensy (dd_voice)
maintainer=rytm-teensy
sentence=digidrum's TRX and EFM machines for the DIGIDRUM voice
paragraph=
category=Signal Input/Output
url=https://github.com/soejrd/digidrum
architectures=*
P
echo "set up $L"
