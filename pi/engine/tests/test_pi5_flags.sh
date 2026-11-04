#!/bin/bash
# pi/engine/tests/test_pi5_flags.sh - the Pi 5 release kernel has the USB-C gadget (computer audio) and not the engine
# test mode (docs/pi5.md "USB device side", "Engine test mode"), checked without building a kernel:
#   1. build.sh's flags (BUILD_FLAGS=1: printed, nothing written): RASPPI=5 is PI5_GADGET=1 PI5_TESTMODE=0 with the
#      GADGET default sidecar; PI5_TESTMODE=1 needs PI5_GADGET=1.
#   2. the Makefile's flags (make print-flags, a stand-in CIRCLEHOME): the release build defines neither
#      SIDECAR_NO_GADGET nor SIDECAR_PI5_TESTMODE, links the gadget and not card_guard.o, no --wrap; the test build all of it.
#   3. the sources (pi5_flags_src.py): with the release build's macros no line that the compiler sees names the test
#      mode (testsecs, /usbtest.log, TestLog, the card gate), and the gadget is started; with SIDECAR_PI5_TESTMODE it is there.
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
echo "test_pi5_flags: ok"
