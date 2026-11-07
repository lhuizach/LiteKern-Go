#!/usr/bin/env bash
# LiteKern Go, phone: what goes into the phone's system from outside Buildroot
# (linux/build.sh runs this with LKX_PHONE=1, before Buildroot builds the image):
#
#   the kernel's modules   from phone/build-kernel.sh (~/lkx-phone/out/modules), with their
#                          depmod files, so eudev and modprobe find the screen and touch drivers
#   Wi-Fi firmware         ~/lkx-phone/stock-wlan/out/wcnss.*, out of the stock package's NON-HLOS.bin
#   firmware               ~/lkx-phone/firmware, copied from a running Nura (the phone's own files):
#                          `scp -r user@PHONE:/lib/firmware/. ~/lkx-phone/firmware/`
#   the shell's disk       build/litekernx.img as a file: the phone has no stick (lkx-start)
#   an ssh key for root    phone/authorized_keys, if there is one: a way in while there's no serial cable
#
# Usage: make-rootfs-overlay.sh BUILDROOT_OUTPUT_DIR
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${1:?usage: make-rootfs-overlay.sh BUILDROOT_OUTPUT_DIR}
O=$OUT/phone-overlay
W=$HOME/lkx-phone
rm -rf "$O"
mkdir -p "$O/lib/modules" "$O/lib/firmware" "$O/usr/share/litekern"
M=$(ls -d "$W"/out/modules/lib/modules/*/ | head -1)
[ -d "$M" ] || { echo "no modules in $W/out/modules: run phone/build-kernel.sh" >&2; exit 1; }
cp -a "$M" "$O/lib/modules/"
rm -f "$O"/lib/modules/*/build "$O"/lib/modules/*/source
# Drivers a phone never uses (the RAM disk has to stay small, phone/README.md):
# filesystems (btrfs, xfs, smb, ...), the firewall, USB HID, cameras, sensors.
VER=$(basename "$M")
for d in fs net/netfilter net/ipv4/netfilter net/ipv6/netfilter drivers/hid drivers/media drivers/iio; do
    rm -rf "$O/lib/modules/$VER/kernel/$d"
done
depmod -b "$O" "$VER"
[ -d "$W/firmware" ] && cp -a "$W/firmware/." "$O/lib/firmware/"
# Wi-Fi: the chip's firmware, from the stock package's NON-HLOS.bin (`image/wcnss.*`),
# not from the phone's partitions and not in git; the drivers ask for its
# settings file by this path.
cp "$W"/stock-wlan/out/wcnss.* "$O/lib/firmware/"
mkdir -p "$O/lib/firmware/wlan/prima"
ln -sf ../../postmarketos/wlan/prima/WCNSS_qcom_wlan_nv.bin "$O/lib/firmware/wlan/prima/"
cp build/litekernx.img "$O/usr/share/litekern/disk.img"
if [ -f phone/authorized_keys ]; then
    mkdir -p "$O/root/.ssh"
    install -m 600 phone/authorized_keys "$O/root/.ssh/authorized_keys"
    chmod 700 "$O/root/.ssh"
fi
echo "phone overlay: $(du -sh "$O" | cut -f1) ($(ls "$O/lib/modules"), $(find "$O/lib/firmware" -type f | wc -l) firmware files)"
