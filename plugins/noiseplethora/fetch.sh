#!/bin/bash
# plugins/noiseplethora/fetch.sh - Befaco's Noise Plethora at a pinned commit into
# vendor/ (git-ignored). Plugins: GPL-3.0-or-later (one CC BY-SA 4.0); framework:
# CC BY-NC-SA 3.0 (not used). The .rmp built from it is GPL-3.0-or-later.
set -e; cd "$(dirname "$0")"
mkdir -p vendor
[ -d vendor/np ] || git clone -q https://github.com/Befaco/Noise_plethora.git vendor/np
git -C vendor/np checkout -q 6dea19646f8acefa81cd12314fbb79c7776f1023
echo "Noise Plethora at $(git -C vendor/np rev-parse --short HEAD)"
