#!/bin/bash
# core/tests/bench/bench.sh TAG [OPT] - bench_render with GCC and the Pi's flags, on this Mac's Apple core, in the rytm-circle
# image (Debian's aarch64 gcc 12: the same "very cheap" -O2 vectoriser as the kernel's arm-none-elf 13.3; its libm is glibc's).
# Prints the table; TAG.prof the PC samples (bench_prof.py). Timings are an Apple core's, for ranking, not an A72's.
set -euo pipefail
cd "$(dirname "$0")/../.."
R=$(git rev-parse --show-toplevel)
TAG=${1:-pi}; OPT=${2:--O2}
docker run --rm -v "$R:$R" -w "$PWD" rytm-circle sh -c "make -s -j8 -f tests/bench/bench.mk PICK=pi TAG=$TAG OPT='$OPT' ${HOT:+HOT='$HOT'} build/bench-$TAG/bench_render && build/bench-$TAG/bench_render -n ${REPEATS:-3} -p build/bench-$TAG/prof.txt"
