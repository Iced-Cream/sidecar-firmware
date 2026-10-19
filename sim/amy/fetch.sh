#!/bin/bash
# AMY (shorepine/amy, MIT) at a pinned commit in sim/amy/vendor (git-ignored);
# none of its own build scripts run. Prints the checkout's path.
set -e
D=$(cd "$(dirname "$0")" && pwd); V=$D/vendor/amy
if [ ! -d "$V/src" ]; then
  mkdir -p "$D/vendor"
  git clone --quiet https://github.com/shorepine/amy.git "$V"
fi
git -C "$V" checkout --quiet 69a146930cbf2c52abb5ad92c26d2157f9095a6d
echo "$V"
