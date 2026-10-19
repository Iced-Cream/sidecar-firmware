#!/bin/bash
# tools/um_starter.sh [CARD_DIR] - the starter user machines (plugins/sc_*) built for the Pi (`make a64`: RMPA
# packages; aarch64-none-elf-gcc from PATH or the rytm-circle Docker image) and, with CARD_DIR (the Pi's SD card
# mounted, or any folder), copied to CARD_DIR/Plugins/<name>.rmp. docs/user-machines-pi.md.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
PY=${PY:-$REPO/.venv/bin/python}
[ -x "$PY" ] || PY=$(command -v python3)
for p in sc_acid sc_wavepad sc_metal sc_snare sc_pluck sc_grain; do
  make -s -C "$REPO/plugins/$p" a64 PY="$PY" >/dev/null
  echo "plugins/$p/$p.a64.rmp  $(wc -c < "$REPO/plugins/$p/$p.a64.rmp" | tr -d ' ') B"
done
if [ $# -ge 1 ]; then
  mkdir -p "$1/Plugins"
  for p in sc_acid sc_wavepad sc_metal sc_snare sc_pluck sc_grain; do cp "$REPO/plugins/$p/$p.a64.rmp" "$1/Plugins/$p.rmp"; done
  echo "copied to $1/Plugins/"
fi
