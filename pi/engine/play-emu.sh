#!/bin/bash
# pi/engine/play-emu.sh - play the Rytm emulator window yourself, with the Pi engine as its Overbridge host over the
# Ethernet cable (the Phase 1 setup of pi/engine/tests/phase1.py, interactive).
#   1. scratch copies of the emulator base (rytm-emu sections/us_us1.v2.snap + .img; the base is never written;
#      phase1.py base regenerates it when missing)
#   2. dtboot prepares them: four tracks on Pi machines by DIN CC 15 (BD PLAITS 44, SD 6-OP FM 40, LT DSP SAMPLE 45,
#      CH DAISYSP 51, as sim/obhost.py --ext), LT's SMP = 1 (CC 27), and with PATTERN=1 (default) a simple pattern
#      in A01 (cleared first: BD 1 9, SD 5 13, LT 3 11, CH 1 5 9 13), stopped
#   3. the window: rytm FW --snapshot --card --usb-host --ob-link 0.0.0.0:7301
#   4. the Pi (serial console): notes=0 out=tracks link=tcp:MAC_IP:7301; waits for "host connected"; pool slot 1 = sine440.wav
#   5. "now play"; when the window closes (or ^C), the Pi goes back to :link=usb
# Usage: pi/engine/play-emu.sh            (env: RYTM_EMU, MAC_IP, PI_IP, PATTERN=0|1, KEEP=1 keeps the scratch dir)
# Defaults: rytm-emu beside this checkout; MAC_IP/PI_IP the link-local pair the Pi's kernel was built with
# (169.254.61.59 / 169.254.61.60). Needs: rytm-emu built (cargo build --release), fw/AR173_us_us1.syx, the Pi running
# the engine kernel on /dev/cu.usbserial-*, cabled. Nothing is flashed.
set -uo pipefail
cd "$(dirname "$0")"
REPO=$(git rev-parse --show-toplevel)
EMU=${RYTM_EMU:-$REPO/../rytm-emu}
EMU=$(cd "$EMU" && pwd) || { echo "no rytm-emu at $EMU (RYTM_EMU=...)"; exit 2; }
MAC_IP=${MAC_IP:-169.254.61.59}
PI_IP=${PI_IP:-169.254.61.60}
PATTERN=${PATTERN:-1}
FW=$EMU/fw/AR173_us_us1.syx
BASE=$EMU/sections/us_us1.v2
for f in "$EMU/target/release/rytm" "$EMU/target/release/dtboot" "$FW"; do
  [ -e "$f" ] || { echo "missing $f (cargo build --release in rytm-emu; the mod build in fw/)"; exit 2; }
done
PORT=$(ls /dev/cu.usbserial-* 2>/dev/null | head -1)
[ -n "$PORT" ] || { echo "no serial adapter (/dev/cu.usbserial-*): the Pi's console"; exit 2; }
ifconfig | grep -q "inet $MAC_IP " || echo "warning: no interface at $MAC_IP on this Mac (MAC_IP=...): the Pi will not reach the link"

if [ ! -e "$BASE.snap" ] || [ ! -e "$BASE.img" ]; then
  (cd "$REPO" && uv run python pi/engine/tests/phase1.py base --if-needed) || { echo "phase1.py base failed"; exit 1; }
fi
D=$(mktemp -d -t play-emu)
echo "scratch: $D"
cp "$BASE.snap" "$D/base.snap" && cp "$BASE.img" "$D/play.img" || { echo "copying the base failed"; exit 1; }

# ---- 2. the tracks (and the pattern), into $D/play.snap. Clocks are instructions (200M = 1 s of the Rytm).
B=$("$EMU/target/release/dtboot" "$FW" --load "$D/base.snap" --instr 1M 2>&1 | sed -n 's/^restored .* at clock \([0-9]*\)$/\1/p')
[ -n "$B" ] || { echo "the base copy does not load (rm $BASE.* and rerun: phase1.py base regenerates it)"; exit 1; }
M=$((B / 1000000))   # in M
ARGS=() T=$((M + 200))
at() { ARGS+=(--press "$1@${T}M"); T=$((T + ${2:-60})); }   # at KEY [GAP]: a press, the next GAP M later
if [ "$PATTERN" = 1 ]; then
  ARGS+=(--press "FUNC@${T}M:120M" --press "PLAY@$((T + 60))M"); T=$((T + 200))   # FUNC + PLAY: CLEAR the pattern
  at RECORD 100
  for spec in "BD 1 9" "SD 5 13" "LT 3 11" "CH 1 5 9 13"; do
    set -- $spec; at "$1" 120; shift
    for s in "$@"; do at "T$s"; done
  done
  at RECORD 100   # grid recording off; stopped (PLAY starts it)
fi
# CC 15 (machine) on each track's channel (BD 1, SD 2, LT 6, CH 9), CC 27 (LT's SMP) = 1: 31250 baud, one message each
ARGS+=(--midi "b00f2c@${T}M" --midi "b10f28@$((T + 20))M" --midi "b50f2d@$((T + 40))M" --midi "b80f33@$((T + 60))M" \
  --midi "b51b01@$((T + 80))M")
T=$((T + 400))
OUT=$("$EMU/target/release/dtboot" "$FW" --load "$D/base.snap" --card "$D/play.img" --card-save --instr "$((T - M))M" \
  --every 100000M --save "$D/play.snap" "${ARGS[@]}" 2>&1)
if [ $? -ne 0 ] || ! [ -s "$D/play.snap" ] || printf '%s' "$OUT" | grep -q FAULT; then
  printf '%s\n' "$OUT" | grep -v '[▄▀█]' | tail -15; echo "dtboot (preparing the tracks) failed"; exit 1
fi
rm -f "$D/base.snap"

# ---- 4. the Pi's console: one reader for the whole session (log $D/pi.log), commands through the FIFO $D/pi.in
mkfifo "$D/pi.in"
(cd "$REPO" && exec uv run python - "$PORT" "$D/pi.log" "$D/pi.in" <<'EOF'
import os, sys, threading, time
import serial
port, log, fifo = sys.argv[1:4]
s = serial.Serial(port, 921600, timeout=0.2)
def cmds():
    while True:
        with open(fifo) as f:
            for c in f:
                c = c.strip()
                if c == "quit":
                    time.sleep(1.5)
                    os._exit(0)
                if c:
                    s.write(b":" + c.encode() + b"\n")
                    time.sleep(0.2)
threading.Thread(target=cmds, daemon=True).start()
with open(log, "w") as f:
    while True:
        l = s.readline().decode(errors="replace").strip()
        if l:
            f.write(l + "\n")
            f.flush()
EOF
) &
PI_PID=$!
exec 3<>"$D/pi.in"   # held open: the reader's FIFO stays readable
pi() { printf '%s\n' "$@" >&3; }

cleanup() {
  trap - EXIT INT TERM
  [ -n "${EMU_PID:-}" ] && kill "$EMU_PID" 2>/dev/null
  echo "Pi: :link=usb"
  pi link=usb quit
  local n=0; while kill -0 "$PI_PID" 2>/dev/null && [ $n -lt 30 ]; do sleep 0.1; n=$((n + 1)); done
  kill "$PI_PID" 2>/dev/null
  grep -E 'link: usb' "$D/pi.log" | tail -1
  echo "logs: $D/emu.err $D/pi.log"
  [ "${KEEP:-0}" = 1 ] || { rm -f "$D/play.snap" "$D/play.img" "$D/play.img.bak" "$D/pi.in"; }
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ---- 3. the window
(cd "$EMU" && exec ./target/release/rytm "$FW" --snapshot "$D/play.snap" --card "$D/play.img" --usb-host \
  --ob-link 0.0.0.0:7301 2>"$D/emu.err") &
EMU_PID=$!
n=0; until grep -q 'overbridge link: listening' "$D/emu.err" 2>/dev/null; do
  kill -0 "$EMU_PID" 2>/dev/null || { tail -5 "$D/emu.err"; echo "the emulator exited"; exit 1; }
  sleep 0.2; n=$((n + 1)); [ $n -lt 100 ] || { echo "the emulator's link did not start ($D/emu.err)"; exit 1; }
done

pi notes=0 out=tracks "link=tcp:$MAC_IP:7301"   # explicit: the kernel's built-in EMUHOST may differ
n=0; until grep -q 'overbridge link: host connected from' "$D/emu.err"; do
  kill -0 "$EMU_PID" 2>/dev/null || { echo "the window was closed"; exit 0; }
  sleep 0.5; n=$((n + 1))
  [ $n -lt 60 ] || { tail -3 "$D/pi.log"; echo "no host after 30 s: the cable, MAC_IP $MAC_IP / PI_IP $PI_IP, the Pi's kernel"; exit 1; }
done
grep -m1 'host connected from' "$D/emu.err"
sleep 4
pi "pool=1:pi-test/sine440.wav"   # after the emulator's project (10): the pool is that project's
cat <<EOM

==== now play: the emulator window, the Pi ($PI_IP) as its Overbridge host
  pads  BD = Plaits, SD = 6-op FM, LT = DSP sample (sine440), CH = DaisySP (the Pi's machines; others are the stubbed analog voices)
  PLAY  starts the prepared pattern in A01 (PATTERN=$PATTERN); STOP stops it
  knobs drag on a knob (or click it, then the arrow keys) on a Pi track: its machine's parameters (SRC page)
  stop  close the window (or ^C here): the Pi goes back to :link=usb
  logs  $D/emu.err (link reports every 10 s), $D/pi.log (the Pi's console)
EOM
while kill -0 "$EMU_PID" 2>/dev/null; do sleep 1; done
EMU_PID=
echo "the window was closed"
