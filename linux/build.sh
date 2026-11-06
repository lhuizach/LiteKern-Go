#!/usr/bin/env bash
# LiteKern Go — builds the VM image, build-linux/litekern-go.img
# (from LiteKern X's Linux build; ROADMAP.md). In WSL:
#
#   bash linux/build.sh            everything (the first time: 1-2 hours)
#   bash linux/build.sh toolchain  just Buildroot's compiler (the slow part)
#   bash linux/build.sh image      lkx-shell, the apps and the image again
#                                  (after a change to them: minutes)
#   LKX_ARCH=aarch64 bash linux/build.sh ...
#                                  the same for ARM (phase 4: the phone's
#                                  Cortex-A53), build-linux/litekern-go-aarch64.img
#                                  and build-linux/aarch64/Image, booted in
#                                  qemu-system-aarch64 (linux/vm.sh qemu-arm)
#
# Buildroot and its output live in ~/lkx-buildroot (WSL's own disk: faster than
# the Windows drive, and Buildroot won't build in a path with spaces).
#
# The stick keeps the kernel version's layout (the Makefile's): the ramdisk
# (apps, wallpapers) at sector 4096 and the FAT32 partition at 16384, so
# lkx-shell opens the stick just as it opens build/litekernx.img on the PC.
# What changes: the boot code. SYSLINUX's MBR starts the FAT32 partition's
# SYSLINUX, which loads Linux from its /syslinux folder. The system is a third
# partition, ext4, after the FAT32 one (ROOT_LBA).
set -euo pipefail
cd "$(dirname "$0")/.."
# LiteKern Go's folder has a space in its name ("LiteKern Go"), which
# Buildroot refuses: build through a link without one.
if [[ "$(pwd)" == *" "* ]]; then
    ln -sfn "$(pwd)" "$HOME/litekern-go"
    cd "$HOME/litekern-go"
fi
REPO=$(pwd)

BR_VERSION=2026.02.3
SYSLINUX_VERSION=6.03
# Buildroot itself and its downloads are shared with LiteKern X's build;
# LiteKern Go's output is its own, one per CPU (x86_64 for the VMs, aarch64
# for the phone: LKX_ARCH).
ARCH=${LKX_ARCH:-x86_64}
WORK=$HOME/lkx-buildroot
BR=$WORK/buildroot-$BR_VERSION
OUT=$WORK/output-go-$ARCH
SYSLINUX=$WORK/syslinux-$SYSLINUX_VERSION
case $ARCH in
x86_64)
    IMAGE=build-linux/litekern-go.img DEFCONFIG=linux/buildroot.defconfig KFRAG=linux/kernel.config
    LBUILD=build-target ;;
aarch64)
    IMAGE=build-linux/litekern-go-aarch64.img DEFCONFIG=linux/buildroot-aarch64.defconfig
    KFRAG=linux/kernel-aarch64.config LBUILD=build-target-aarch64 ;;
*)
    echo "LKX_ARCH: x86_64 or aarch64" >&2
    exit 2 ;;
esac
# LKX_WEB=1: with WPE WebKit (linux/buildroot-web.fragment), in its own output.
if [ "${LKX_WEB:-0}" = 1 ]; then
    OUT=$WORK/output-go-web-$ARCH
    mkdir -p build-linux
    cat "$DEFCONFIG" linux/buildroot-web.fragment > "build-linux/buildroot-web-$ARCH.defconfig.new"
    cmp -s "build-linux/buildroot-web-$ARCH.defconfig.new" "build-linux/buildroot-web-$ARCH.defconfig" ||
        cp "build-linux/buildroot-web-$ARCH.defconfig.new" "build-linux/buildroot-web-$ARCH.defconfig"
    DEFCONFIG=build-linux/buildroot-web-$ARCH.defconfig
fi
# LKX_PHONE=1 (with LKX_ARCH=aarch64): the phone's system, as a RAM disk for
# `fastboot boot` (phone/README.md): no kernel, no disk image.
PHONE=${LKX_PHONE:-0}
if [ "$PHONE" = 1 ]; then
    [ "$ARCH" = aarch64 ] || { echo "LKX_PHONE needs LKX_ARCH=aarch64" >&2; exit 2; }
    OUT=$WORK/output-go-phone
    DEFCONFIG=linux/buildroot-phone.defconfig
    LBUILD=build-target-phone
fi
FAT_OFFSET=$((16384 * 512))
ROOT_LBA=$((16384 + 64 * 2048))     # after the 64 MiB FAT32 partition: 72 MiB

step=${1:-all}
mkdir -p "$WORK/dl"
# WSL adds Windows' folders to PATH ("Program Files"...): Buildroot refuses
# spaces in it, and needs none of them.
PATH=$(printf '%s' "$PATH" | tr ':' '\n' | grep -v ' ' | grep -v '^/mnt/' | paste -sd: -)
# Ubuntu 26.04's coreutils are a Rust rewrite, which Buildroot turns away; the
# GNU ones are still installed, as gnucp, gnuinstall...: first in our PATH,
# by their own names.
mkdir -p "$WORK/gnubin"
for f in /usr/bin/gnu*; do
    [ -x "$f" ] && ln -sf "$f" "$WORK/gnubin/$(basename "$f" | sed 's/^gnu//')"
done
PATH=$WORK/gnubin:$PATH
export PATH

fetch() {   # URL DIR: download and unpack once
    [ -d "$2" ] && return
    echo "== downloading $(basename "$1")"
    wget -q -O "$WORK/dl/$(basename "$1")" "$1"
    tar -xf "$WORK/dl/$(basename "$1")" -C "$WORK"
}
fetch "https://buildroot.org/downloads/buildroot-$BR_VERSION.tar.xz" "$BR"
fetch "https://www.kernel.org/pub/linux/utils/boot/syslinux/syslinux-$SYSLINUX_VERSION.tar.xz" "$SYSLINUX"

# Buildroot's configuration: linux/buildroot.defconfig, with linux/ as its
# external tree (kernel.config, overlay/).
if [ ! -f "$OUT/.config" ] || [ "$DEFCONFIG" -nt "$OUT/.config" ]; then
    make -s -C "$BR" O="$OUT" BR2_EXTERNAL="$REPO/linux" BR2_DEFCONFIG="$REPO/$DEFCONFIG" defconfig
fi

echo "== the toolchain"
make -s -C "$BR" O="$OUT" toolchain
[ "$step" = toolchain ] && exit 0

# The host side first: the disk image (its ramdisk and FAT32) and the files
# generated for lkx-shell (fonts, icons, cursors).
echo "== the host build (build/litekernx.img)"
make -s build/litekernx.img

# lkx-shell and the apps, with Buildroot's compiler, into the overlay.
echo "== lkx-shell and the apps, for $ARCH"
CROSS=$OUT/host/bin/$ARCH-buildroot-linux-gnu-gcc
make -s -f platform/Makefile CC="$CROSS" ARCH_FLAGS= LBUILD=$LBUILD KERNEL_BUILD=0
rm -rf "$OUT/lkx-overlay"
mkdir -p "$OUT/lkx-overlay/usr/bin" "$OUT/lkx-overlay/usr/lib/litekern/apps"
cp $LBUILD/lkx-shell "$OUT/lkx-overlay/usr/bin/"
for app in apps/*/; do
    n=$(basename "$app")
    mkdir -p "$OUT/lkx-overlay/usr/lib/litekern/apps/$n"
    cp "$LBUILD/apps/$n/$n" "$OUT/lkx-overlay/usr/lib/litekern/apps/$n/"
done

[ "$PHONE" = 1 ] && bash phone/make-rootfs-overlay.sh "$OUT"

echo "== Linux and the system"
# Buildroot doesn't notice a changed kernel.config by itself.
if [ "$PHONE" != 1 ] && [ -f "$OUT/.lkx-kernel-config" ] && [ "$KFRAG" -nt "$OUT/.lkx-kernel-config" ]; then
    make -s -C "$BR" O="$OUT" linux-reconfigure
fi
touch "$OUT/.lkx-kernel-config"
if [ -f "$OUT/.lkx-busybox-config" ] && [ linux/busybox.fragment -nt "$OUT/.lkx-busybox-config" ]; then
    make -s -C "$BR" O="$OUT" busybox-reconfigure
fi
touch "$OUT/.lkx-busybox-config"
make -s -C "$BR" O="$OUT"
if [ "$PHONE" = 1 ]; then
    echo "== done: $OUT/images/rootfs.cpio.xz ($(du -h "$OUT/images/rootfs.cpio.xz" | cut -f1)); next: phone/make-ramboot.sh"
    exit 0
fi

echo "== the image"
mkdir -p build-linux
cp build/litekernx.img "$IMAGE"
# The system: partition 3, ext4 (tools/mkmbr.py wrote the first two).
python3 - "$IMAGE" "$OUT/images/rootfs.ext4" "$ROOT_LBA" <<'EOF'
import os, struct, sys
img, root, lba = sys.argv[1], sys.argv[2], int(sys.argv[3])
sectors = os.path.getsize(root) // 512
with open(img, "r+b") as f:
    f.seek(446 + 2 * 16)
    chs = bytes.fromhex("feffff")           # the "use LBA" marker
    f.write(bytes([0]) + chs + bytes([0x83]) + chs + struct.pack("<II", lba, sectors))
EOF
dd if="$OUT/images/rootfs.ext4" of="$IMAGE" bs=1M seek=$((ROOT_LBA / 2048)) conv=notrunc,sparse status=none
if [ "$ARCH" = aarch64 ]; then
    # QEMU's virt machine boots the kernel it's given (-kernel), the disk the
    # same stick as the VM's. The phone gets its own boot image later (phase 7).
    mkdir -p build-linux/aarch64
    cp "$OUT/images/Image" build-linux/aarch64/Image
    echo "== done: $IMAGE ($(du -h "$IMAGE" | cut -f1)), build-linux/aarch64/Image"
    exit 0
fi
python3 - "$IMAGE" "$SYSLINUX/bios/mbr/mbr.bin" <<'EOF'
import sys
img, mbr = sys.argv[1], open(sys.argv[2], "rb").read()
with open(img, "r+b") as f:
    sector = bytearray(f.read(512))
    sector[:len(mbr)] = mbr                 # SYSLINUX's boot code; the partition table stays
    for i in range(4):                      # the FAT32 partition (at 16384) is the one it starts
        e = 446 + i * 16
        lba = int.from_bytes(sector[e + 8:e + 12], "little")
        sector[e] = 0x80 if lba == 16384 else 0x00
    f.seek(0)
    f.write(sector)
EOF
mmd -i "$IMAGE@@$FAT_OFFSET" ::/syslinux 2>/dev/null || true
mcopy -o -i "$IMAGE@@$FAT_OFFSET" "$OUT/images/bzImage" ::/syslinux/bzImage
# The same kernel for EFI firmware: the VirtualBox VM
# boots it with no bootloader, so nothing but the logo shows (linux/kernel.config).
mmd -i "$IMAGE@@$FAT_OFFSET" ::/EFI ::/EFI/BOOT 2>/dev/null || true
mcopy -o -i "$IMAGE@@$FAT_OFFSET" "$OUT/images/bzImage" ::/EFI/BOOT/BOOTX64.EFI
mcopy -o -i "$IMAGE@@$FAT_OFFSET" linux/syslinux.cfg ::/syslinux/syslinux.cfg
"$SYSLINUX/bios/mtools/syslinux" --offset "$FAT_OFFSET" --directory /syslinux --install "$IMAGE"
echo "== done: $IMAGE ($(du -h "$IMAGE" | cut -f1))"
