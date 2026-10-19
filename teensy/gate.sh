#!/bin/bash
# teensy/gate.sh quick|full|record - the engine core extraction's refactor gate on the Teensy
# (docs/superpowers/plans/2026-10-14-engine-core-extraction.md).
#   quick   the serial build: compile + upload, its sizes against sim/golden/baseline.json, the golden renders
#   full    quick, then sim/rmp_e2e.py (serial), the host build (RYTM_USB_HOST=1) with sizes and rmp_e2e.py --host,
#           plaits_bench (printed: compare it by eye with the baseline's, docs/core.md), and the plugin-less
#           build (RYTM_PLUGINS=0): compile + upload + boot to an ITCM line, its sizes not compared
#   record  the baseline: both builds' sizes recorded, then full's checks (Task 1 only; the plugin-less
#           build is checked but its sizes are not recorded)
# Always ends with the serial voiceloop on the Teensy. Logs in ${TMPDIR:-/tmp}/rytm-gate/.
set -eo pipefail
D=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$D/.." && pwd)
MODE=${1:-quick}
L=${TMPDIR:-/tmp}/rytm-gate
mkdir -p "$L"
fail() { echo "GATE $MODE FAIL: $*"; (cd "$D" && ./run_sketch.sh voiceloop 1 >/dev/null 2>&1 || true); exit 1; }
sizes() {   # $1 log, $2 serial|host
  if [ "$MODE" = record ]; then (cd "$R" && uv run python sim/core_golden.py sizes "$1" "$2" --record) || fail "sizes --record ($2)"
  else (cd "$R" && uv run python sim/core_golden.py sizes "$1" "$2") || fail "sizes ($2)"; fi
}
(cd "$D" && ./run_sketch.sh voiceloop 3) 2>&1 | tee "$L/serial.log" || fail "serial build"
grep -q '^ITCM' "$L/serial.log" || fail "no ITCM line in the serial build's log"
sizes "$L/serial.log" serial
if [ "$MODE" != record ]; then
  (cd "$R" && uv run python sim/core_golden.py check-teensy) 2>&1 | tee "$L/golden.log" || true
  grep -q 'CHECK-TEENSY PASS' "$L/golden.log" || fail "golden renders"
fi
if [ "$MODE" = full ] || [ "$MODE" = record ]; then
  (cd "$R" && uv run python sim/rmp_e2e.py) 2>&1 | tee "$L/e2e-serial.log" || true
  grep -q '^ALL PASS' "$L/e2e-serial.log" || fail "rmp_e2e.py (serial)"
  (cd "$D" && RMP_EXTRA_FLAGS="-DRYTM_USB_HOST=1" ./run_sketch.sh voiceloop 3) 2>&1 | tee "$L/host.log" || fail "host build"
  sizes "$L/host.log" host
  (cd "$R" && uv run python sim/rmp_e2e.py --host) 2>&1 | tee "$L/e2e-host.log" || true
  grep -q '^ALL PASS' "$L/e2e-host.log" || fail "rmp_e2e.py --host"
  (cd "$D" && ./run_sketch.sh plaits_bench 40) 2>&1 | tee "$L/plaits_bench.log" || fail "plaits_bench"
  (cd "$D" && RMP_EXTRA_FLAGS="-DRYTM_PLUGINS=0" ./run_sketch.sh voiceloop 3) 2>&1 | tee "$L/noplugins.log" \
    || fail "plugin-less build (RYTM_PLUGINS=0)"
  grep -q '^ITCM' "$L/noplugins.log" || fail "no ITCM line in the plugin-less build's log"
  (cd "$D" && ./run_sketch.sh voiceloop 1) >/dev/null 2>&1 || fail "serial build back on the Teensy"
fi
echo "GATE $MODE PASS"
