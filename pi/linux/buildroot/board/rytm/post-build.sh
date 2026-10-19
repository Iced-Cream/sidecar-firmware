#!/bin/sh
# Install the rt-tuning scripts (single source of truth: ../../rt-tuning) into the target.
set -e
TARGET_DIR="$1"
HERE="$(cd "$(dirname "$0")" && pwd)"
RT="$HERE/../../../rt-tuning"
install -D -m 0755 "$RT/S10rt-tune" "$TARGET_DIR/etc/init.d/S10rt-tune"
install -D -m 0755 "$RT/S90engine" "$TARGET_DIR/etc/init.d/S90engine"
