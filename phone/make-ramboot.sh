#!/usr/bin/env bash
# LiteKern Go, phone: a boot image of our own kernel and our own system, for
# `fastboot boot` (lk2nd starts it from RAM: nothing is written to the phone).
#
#   LKX_ARCH=aarch64 LKX_PHONE=1 bash linux/build.sh     (the system: rootfs.cpio.xz)
#   bash phone/build-kernel.sh                           (the kernel and its modules)
#   bash phone/make-ramboot.sh
#
# Out: ~/lkx-phone/out/boot-litekern-go.img. Same layout and addresses as Nura's
# boot image (its header is the model); our cmdline: the screen's text console
# first, so a fault shows on the panel, and the serial port.
set -euo pipefail
cd "$(dirname "$0")"
W=$HOME/lkx-phone
O=$W/out
CPIO=${1:-$HOME/lkx-buildroot/output-go-phone/images/rootfs.cpio.xz}
for f in "$O/Image.gz" "$O/msm8916-samsung-a5u-eur.dtb" "$W/nura/boot.img" "$CPIO"; do
    [ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
cat "$O/Image.gz" "$O/msm8916-samsung-a5u-eur.dtb" > "$O/kernel-dtb"
python3 bootimg.py pack "$W/nura/boot.img" "$O/boot-litekern-go.img" "$O/kernel-dtb" "$CPIO" \
    "console=tty0 console=ttyMSM0,115200 loglevel=5"
python3 bootimg.py info "$O/boot-litekern-go.img"
sha256sum "$O/boot-litekern-go.img" | cut -c1-16
