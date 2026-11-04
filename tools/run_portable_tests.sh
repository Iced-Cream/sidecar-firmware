#!/bin/bash
# tools/run_portable_tests.sh - the tests that need no hardware, no firmware and no cross toolchain; the CI's "test"
# check runs exactly this (Linux and macOS):
#   - core: the generic engine core (make -C core test);
#   - pi/engine/tests: the Pi engine's portable parts (ASan/UBSan; the FatFs ones only where a Circle checkout is);
#   - pi/engine/tests/*.test.mjs: the web page's logic (Node 20 or newer).
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
CXX=${CXX:-c++}

make -C "$R/core" CXX="$CXX" test

# The FatFs tests copy Circle's addon/fatfs (pi/circle/build.sh's checkout); without it they are named as skipped.
PT=$R/pi/engine/tests
skip=()
[ -f "$R/pi/circle/build/circle/addon/fatfs/ff.c" ] || skip+=(test_stor_fatfs test_stor_fatfs_exfat test_stor_fatfs_lfn3)
tests=()
for f in "$PT"/test_*.cpp test_stor_fatfs_exfat test_stor_fatfs_lfn3; do
  t=$(basename "$f" .cpp)
  [[ " ${skip[*]} " == *" $t "* ]] || tests+=("build/$t")
done
[ ${#skip[@]} -eq 0 ] || echo "pi/engine/tests: skipped here: ${skip[*]}"
make -C "$PT" CXX="$CXX" TESTS="${tests[*]}"

(cd "$R" && node --test pi/engine/tests/*.test.mjs)
echo "ALL PORTABLE TESTS PASS"
