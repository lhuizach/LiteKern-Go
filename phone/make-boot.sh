#!/usr/bin/env bash
# LiteKern Go, phase 6: a boot image with our kernel and everything else Nura's:
# the same layout and cmdline, the same initramfs. One change from the image
# we'd have flashed in phase 5, so a failure points at the kernel.
#
#   bash phone/build-kernel.sh, then bash phone/make-boot.sh
#
# Out: ~/lkx-phone/out/boot-our-kernel.img. This makes a file; flashing it is
# a separate step, only with the owner's confirmation (CLAUDE.md).
set -euo pipefail
cd "$(dirname "$0")"
W=$HOME/lkx-phone
O=$W/out
XZ=${NURA_BOOT:-/mnt/c/Users/lhuir/Downloads/20261030-1211-nura-v26.06-phosh-samsung-a5-boot.img.xz}
mkdir -p "$W/nura"
[ -f "$W/nura/boot.img" ] || xz -dc "$XZ" > "$W/nura/boot.img"
python3 bootimg.py unpack "$W/nura/boot.img" "$W/nura/parts"
# deviceinfo_append_dtb: the device tree straight after the compressed kernel
cat "$O/Image.gz" "$O/msm8916-samsung-a5u-eur.dtb" > "$O/kernel-dtb"
python3 bootimg.py pack "$W/nura/boot.img" "$O/boot-our-kernel.img" "$O/kernel-dtb" "$W/nura/parts/ramdisk"
python3 bootimg.py info "$O/boot-our-kernel.img"
