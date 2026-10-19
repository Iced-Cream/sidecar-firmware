#!/bin/bash
# The Teensy's curated engine sources - libraries Plaits, Juno, DaisySP, Digidrum, Sampler as teensy/setup_*_lib.sh
# make them - copied for a non-Arduino build into DEST/<Lib>/, with the Teensy-only bits undone (PROGMEM and
# <avr/pgmspace.h> on Plaits' tables, the .flashmem section on Voice::Init; as pi/circle/spike3/setup_plaits.sh).
# The same sources the Teensy builds, so the Mac and the Pi play what the Teensy plays.
#   core/engines/setup_engines.sh DEST      (core/Makefile: core/build/engines; pi/engine/Makefile: its own)
# Git-ignored output; rerun freely. Runs the Teensy's setup scripts for a missing library.
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
T=$(cd "$D/../../teensy" && pwd)
DEST=${1:?usage: setup_engines.sh DEST}
# remade when missing, or when this repo's own files in it (sim/plaits/fm_machine.h, the user_data.h shim) changed
# (measured: a stale fm_machine.h failed test_content's fm)
"$T/setup_plaits_lib.sh" --if-stale >/dev/null
[ -d "$T/libraries/DaisySP" ] || "$T/setup_daisysp_lib.sh" >/dev/null
[ -d "$T/libraries/Digidrum" ] || "$T/setup_digidrum_lib.sh" >/dev/null
"$T/setup_juno_lib.sh" >/dev/null
"$T/setup_sampler_lib.sh" >/dev/null
rm -rf "$DEST"; mkdir -p "$DEST"
for lib in Plaits Juno DaisySP Digidrum Sampler; do cp -R "$T/libraries/$lib/src" "$DEST/$lib"; done
for f in "$DEST/Plaits/plaits/resources.cc" "$DEST/Plaits/plaits/dsp/speech/lpc_speech_synth_words.cc"; do
  sed -i '' -e '/^#include <avr\/pgmspace.h>$/d' -e 's/\] PROGMEM = {/] = {/' "$f"
done
sed -i '' 's|^__attribute__((section(".flashmem"))) void Voice::Init|void Voice::Init|' "$DEST/Plaits/plaits/dsp/voice.cc"
! grep -rqE 'PROGMEM|pgmspace|section\("\.flashmem"\)' "$DEST/Plaits" || { echo "setup_engines.sh: Teensy-only bits left in Plaits"; exit 1; }
echo "engines in $DEST ($(find "$DEST" -name '*.cc' -o -name '*.cpp' -o -name '*.c' | wc -l | tr -d ' ') sources)"
