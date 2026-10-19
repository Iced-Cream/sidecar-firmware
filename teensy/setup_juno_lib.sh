#!/bin/bash
# The JUNO voice (sim/juno/juno_voice.h, juno_patches.h from AMY's juno.py, MIT)
# as an Arduino library in libraries/Juno. Git-ignored; rerun freely.
set -e
D=$(cd "$(dirname "$0")" && pwd)
L=$D/libraries/Juno
rm -rf "$L"; mkdir -p "$L/src"
cp "$D/../sim/juno/juno_voice.h" "$D/../sim/juno/juno_patches.h" "$L/src/"
cat > "$L/src/Juno.h" <<'H'
// Arduino finds a library by a header at the top of src/: include this first.
#pragma once
#include "juno_voice.h"
H
cat > "$L/library.properties" <<'P'
name=Juno
version=0.0.0
author=rytm-teensy (juno_voice.h); shorepine/amy (Juno-106 patches and curves, MIT)
maintainer=rytm-teensy
sentence=A Juno-106 voice for the JUNO machine
paragraph=
category=Signal Input/Output
url=https://github.com/shorepine/amy
architectures=*
P
echo "set up $L"
