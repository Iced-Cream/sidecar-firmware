#!/bin/bash
# pi/engine/tests/test_pi5_flags.sh - the Pi 5 release kernel has the USB-C gadget (computer audio) and not the engine
# test mode (docs/pi5.md "USB device side", "Engine test mode"), checked without building a kernel:
#   1. build.sh's flags (BUILD_FLAGS=1: printed, nothing written): RASPPI=5 is PI5_GADGET=1 PI5_TESTMODE=0 with the
#      GADGET default sidecar; PI5_TESTMODE=1 needs PI5_GADGET=1.
#   2. the Makefile's flags (make print-flags, a stand-in CIRCLEHOME): the release build defines neither
#      SIDECAR_NO_GADGET nor SIDECAR_PI5_TESTMODE, links the gadget and not card_guard.o, no --wrap; the test build all of it.
#   3. the sources (pi5_flags_src.py): with the release build's macros no line that the compiler sees names the test
#      mode (testsecs, /usbtest.log, TestLog, the card gate), and the gadget is started; with SIDECAR_PI5_TESTMODE it is there.
#   4. usb=mirror on the Pi 5 (docs/usb-mirror.md): build.sh puts the Pi 4's gadget patches on the pi5usb tree too, the two
#      mirror patches last as on the Pi 4; with a Circle checkout at hand (CIRCLE_BUILD or pi/circle/build), the whole
#      Pi 5 stack applied to a copy of it leaves CIRCLE_GADGET_MIRROR and CIRCLE_GADGET_SOFTCONNECT in the gadget header.
set -euo pipefail
cd "$(dirname "$0")"
ENG=$PWD/..
fail() { echo "test_pi5_flags: FAIL: $*"; exit 1; }
CLEAN=(-u RASPPI -u PI5_GADGET -u PI5_TESTMODE -u PI5_UART -u GADGET -u STOR_EXFAT)   # the caller's own build flags left out
flags() { env "${CLEAN[@]}" "$@" BUILD_FLAGS=1 "$ENG/build.sh"; }
has() { grep -qxF "$2" <<<"$1" || fail "$3: expected '$2' in: $(tr '\n' ' ' <<<"$1")"; }

F=$(flags RASPPI=5)
has "$F" "PI5_GADGET=1" "RASPPI=5"; has "$F" "PI5_TESTMODE=0" "RASPPI=5"; has "$F" "GADGET_DEFAULT=sidecar" "RASPPI=5"
has "$F" "TREE=circle-engine-pi5usb" "RASPPI=5"; has "$F" "KIMG=kernel_2712.img" "RASPPI=5"
F=$(flags RASPPI=5 PI5_TESTMODE=1)
has "$F" "PI5_GADGET=1" "PI5_TESTMODE=1"; has "$F" "PI5_TESTMODE=1" "PI5_TESTMODE=1"
has "$F" "STAMP=RASPPI=5 PI5_UART=0 PI5_GADGET=1 PI5_TESTMODE=1" "PI5_TESTMODE=1"
F=$(flags RASPPI=5 PI5_GADGET=0)
has "$F" "PI5_GADGET=0" "PI5_GADGET=0"; has "$F" "TREE=circle-engine-pi5" "PI5_GADGET=0"
F=$(flags RASPPI=4)
has "$F" "GADGET_DEFAULT=sidecar" "RASPPI=4"; has "$F" "PI5_TESTMODE=0" "RASPPI=4"
! flags RASPPI=5 PI5_GADGET=0 PI5_TESTMODE=1 >/dev/null 2>&1 || fail "PI5_TESTMODE=1 without the gadget was accepted"
! flags RASPPI=4 PI5_TESTMODE=1 >/dev/null 2>&1 || fail "PI5_TESTMODE=1 on the Pi 4 was accepted"

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
printf 'RASPPI = 5\n' > "$T/Config.mk"; : > "$T/Rules.mk"
mf() { env "${CLEAN[@]}" make -s -C "$ENG" CIRCLEHOME="$T" "$@" print-flags; }
M=$(mf PI5_GADGET=1)
grep -q -- "-DSIDECAR_NO_GADGET\|-DSIDECAR_PI5_TESTMODE" <<<"$M" && fail "release Makefile flags: $M"
grep -q "^OBJS=.*sidecar_gadget.o" <<<"$M" || fail "release build without the gadget: $M"
grep -q "card_guard.o\|--wrap" <<<"$M" && fail "release build with the card gate: $M"
M=$(mf PI5_GADGET=1 PI5_TESTMODE=1)
grep -q -- "-DSIDECAR_PI5_TESTMODE" <<<"$M" || fail "test build without SIDECAR_PI5_TESTMODE: $M"
grep -q -- "-DSIDECAR_NO_GADGET" <<<"$M" && fail "test build without the gadget: $M"
grep -q "^OBJS=.*card_guard.o" <<<"$M" && grep -q "^LDFLAGS=.*--wrap=f_open" <<<"$M" || fail "test build without the card gate: $M"
M=$(mf PI5_GADGET=0)
grep -q -- "-DSIDECAR_NO_GADGET" <<<"$M" || fail "PI5_GADGET=0 with the gadget: $M"
! mf PI5_GADGET=0 PI5_TESTMODE=1 >/dev/null 2>&1 || fail "the Makefile took PI5_TESTMODE=1 without the gadget"

python3 pi5_flags_src.py "$ENG"

P5=$(grep -E '^ *(P=[^;]*; )?PP=' "$ENG/build.sh" | grep -o 'circle-[a-z0-9-]*\.patch' | tr '\n' ' ')
[ "$P5" = "circle-gadget.patch circle-gadget-wake.patch circle-pi5-gadget.patch circle-gadget-reset.patch circle-gadget-mirror.patch circle-gadget-softconnect.patch circle-gadget-stray.patch " ] \
  || fail "the Pi 5 gadget patches in build.sh: $P5"
P4=$(grep -o 'patch -p1 < /repo/pi/circle/circle-gadget[a-z-]*\.patch' "$ENG/build.sh" | sed 's,.*/,,' | tr '\n' ' ')
[ "$P4" = "circle-gadget.patch circle-gadget-wake.patch circle-gadget-reset.patch circle-gadget-mirror.patch circle-gadget-softconnect.patch circle-gadget-stray.patch " ] \
  || fail "the Pi 4 gadget patches in build.sh: $P4"
C=${CIRCLE_BUILD:-$ENG/../circle/build}/circle
if [ -f "$C/include/circle/usb/gadget/dwusbgadget.h" ]; then
  cp -R "$C/include" "$C/lib" "$T/"
  for f in $P5; do (cd "$T" && patch -s -p1 < "$ENG/../circle/$f") || fail "$f does not apply to the Pi 5 stack"; done
  for m in CIRCLE_GADGET_MIRROR CIRCLE_GADGET_SOFTCONNECT; do
    grep -q "#define $m" "$T/include/circle/usb/gadget/dwusbgadget.h" || fail "the Pi 5 stack without $m"
  done
else
  echo "test_pi5_flags: no Circle checkout at $C: the Pi 5 patch stack not applied (the order checked)"
fi
echo "test_pi5_flags: ok"
