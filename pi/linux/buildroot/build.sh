#!/bin/bash
# Buildroot build in Docker; the tree and outputs live in the named volume "rytm-br" (not in the repo).
#   ./build.sh            configure + build
#   ./build.sh shell      shell in the build container (cd /br/buildroot-*; make menuconfig O=/br/out BR2_EXTERNAL=/pi/buildroot)
#   ./build.sh fetch      copy images from the volume to ./out (gitignored)
set -e
BR_VER=2026.02.3
HERE="$(cd "$(dirname "$0")" && pwd)"
PI_LINUX="$(cd "$HERE/.." && pwd)"
IMG=rytm-br-builder
VOL=rytm-br
docker build -q -t $IMG -f - "$HERE" <<'DOCK'
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends build-essential wget curl ca-certificates cpio unzip rsync bc file python3 libncurses-dev git perl libssl-dev flex bison libelf-dev bzip2 patch gzip xz-utils sed tar findutils diffutils util-linux && rm -rf /var/lib/apt/lists/*
DOCK
RUN() { docker run --rm -e FORCE_UNSAFE_CONFIGURE=1 -v $VOL:/br -v "$PI_LINUX":/pi:ro -w /br $IMG "$@"; }
case "${1:-build}" in
  shell) docker run --rm -it -e FORCE_UNSAFE_CONFIGURE=1 -v $VOL:/br -v "$PI_LINUX":/pi:ro -w /br $IMG bash ;;
  fetch) mkdir -p "$PI_LINUX/out"; docker run --rm -v $VOL:/br -v "$PI_LINUX/out":/o $IMG sh -c 'cp -v /br/out/images/Image /br/out/images/*.dtb /br/out/images/sdcard.img /br/out/images/rootfs.ext4 /o/' ;;
  build)
    RUN bash -ec "
      [ -d buildroot-$BR_VER ] || { wget -q https://buildroot.org/downloads/buildroot-$BR_VER.tar.xz && tar xf buildroot-$BR_VER.tar.xz && rm buildroot-$BR_VER.tar.xz; }
      cd buildroot-$BR_VER
      make O=/br/out BR2_EXTERNAL=/pi/buildroot rytm_pi4_rt_defconfig
      make O=/br/out BR2_EXTERNAL=/pi/buildroot -j\$(nproc)
    " ;;
esac
