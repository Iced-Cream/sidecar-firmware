#!/bin/bash
# core/tests/bench/test_neon_gcc.sh - core/tests/test_neon.cpp with GCC at the Pi's flags, in the rytm-circle image (make -C core
# engines first). Two compilers: Debian's native gcc 12, and the kernel's aarch64-none-elf 13.3, which compiles the test and
# resources, Debian's linking them for Linux (the bare-metal toolchain has no OS to run on). -O2 and -O3: the render files are -O3.
set -euo pipefail
cd "$(dirname "$0")/../.."
R=$(git rev-parse --show-toplevel)
docker run --rm -v "$R:$R" -w "$PWD" rytm-circle sh -c '
set -e
E=build/engines; T=build/test-neon-gcc; mkdir -p $T
I="-I. -Ihost -Itests -I../sdk -I$E/Plaits -I$E/Juno -I$E/DaisySP -I$E/Sampler -I$E/MDKit -DTEST -DCORE_POLY=1 -DCORE_MSAMP=1"
F="-mcpu=cortex-a72 -mlittle-endian -fsigned-char -fno-math-errno -ffp-contract=fast -g -std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -w"
for o in -O2 -O3; do
  for cc in g++ aarch64-none-elf-g++; do
    $cc $F $o $I -include string.h -include math.h -include stdint.h -c -o $T/t.o tests/test_neon.cpp
    $cc $F -O2 $I -c -o $T/r.o $E/Plaits/plaits/resources.cc
    g++ -static -o $T/test_neon $T/t.o $T/r.o -lm
    echo "== $cc $o"; $T/test_neon
  done
done'
