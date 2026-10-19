#!/bin/bash
# Build Plaits (Mutable Instruments, MIT) as a native library for the Teensy
# simulator. shim/plaits/user_data.h stands in for Plaits' own (the 6-OP FM
# machine's banks, fm_machine.h). Sources: Mutable's eurorack repo at a pinned commit (the same one
# rytm-emu's dspbench uses), in vendor/ (git-ignored); nothing is committed here.
set -e
D=$(cd "$(dirname "$0")" && pwd); V=$D/vendor/eurorack
if [ ! -d "$V/plaits" ]; then
  L=$D/../../../rytm-emu/tools/dspbench/vendor/eurorack
  mkdir -p "$D/vendor"
  if [ -d "$L/plaits" ]; then ln -sfn "$L" "$V"
  else
    git clone --quiet https://github.com/pichenettes/eurorack.git "$V"
    git -C "$V" checkout --quiet 08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4
    git -C "$V" submodule update --init stmlib
    git -C "$V/stmlib" checkout --quiet e3bd7c9cc00e4364166f9905c0509b6ffd0535ec
  fi
fi
SRC=$(cd "$V" && find plaits/dsp -name '*.cc' | sort; echo plaits/resources.cc stmlib/dsp/units.cc stmlib/utils/random.cc)
mkdir -p "$D/build"
(cd "$V" && clang++ -std=c++14 -O2 -fPIC -shared -DTEST -I"$D/shim" -I"$D" -I. -o "$D/build/libplaits.dylib" "$D/plaits_c.cc" $SRC)
echo "built $D/build/libplaits.dylib"
