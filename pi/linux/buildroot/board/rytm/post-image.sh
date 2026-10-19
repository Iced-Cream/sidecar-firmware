#!/bin/bash
# Minimal post-image: boot files list -> genimage -> sdcard.img (as Buildroot's raspberrypi post-image.sh).
set -e
BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"
GENIMAGE_CFG="${BINARIES_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"
FILES=()
for i in "${BINARIES_DIR}"/*.dtb "${BINARIES_DIR}"/rpi-firmware/*; do
	FILES+=( "${i#${BINARIES_DIR}/}" )
done
KERNEL=$(sed -n 's/^kernel=//p' "${BINARIES_DIR}/rpi-firmware/config.txt")
FILES+=( "${KERNEL}" )
BOOT_FILES=$(printf '\\t\\t\\t"%s",\\n' "${FILES[@]}")
sed "s|#BOOT_FILES#|${BOOT_FILES}|" "${BOARD_DIR}/genimage.cfg.in" > "${GENIMAGE_CFG}"
ROOTPATH_TMP="$(mktemp -d)"
trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
rm -rf "${GENIMAGE_TMP}"
genimage --rootpath "${ROOTPATH_TMP}" --tmppath "${GENIMAGE_TMP}" --inputpath "${BINARIES_DIR}" \
	--outputpath "${BINARIES_DIR}" --config "${GENIMAGE_CFG}"
exit $?
