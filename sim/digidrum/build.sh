#!/bin/bash
# Build the DIGIDRUM voice (dd_voice.c on soejrd/digidrum, MIT) as a native
# library for the Teensy simulator. digidrum at a pinned commit in vendor/
# (git-ignored); only its machines/ and dsp/ C sources are compiled, none of its
# own build scripts run. Nothing from it is committed here.
set -e
D=$(cd "$(dirname "$0")" && pwd); V=$D/vendor/digidrum
if [ ! -d "$V/machines" ]; then
  mkdir -p "$D/vendor"
  git clone --quiet https://github.com/soejrd/digidrum.git "$V"
fi
git -C "$V" checkout --quiet 2f893f9dbb5e532df3859c0e0468bf9fb826674a
mkdir -p "$D/build"
clang -std=c99 -O2 -fPIC -shared -I"$V" -I"$V/include" -o "$D/build/libdigidrum.dylib" \
  "$D/dd_voice.c" "$V"/machines/*.c "$V"/dsp/*.c
echo "built $D/build/libdigidrum.dylib"
