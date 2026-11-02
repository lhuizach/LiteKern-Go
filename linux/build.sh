#!/usr/bin/env bash
# LiteKern Go — builds the VM image, build-linux/litekern-go.img
# (from LiteKern X's Linux build; ROADMAP.md). In WSL:
#
#   bash linux/build.sh            everything (the first time: 1-2 hours)
#   bash linux/build.sh toolchain  just Buildroot's compiler (the slow part)
#   bash linux/build.sh image      lkx-shell, the apps and the image again
#                                  (after a change to them: minutes)
#
# Buildroot and its output live in ~/lkx-buildroot (WSL's own disk: faster than
# the Windows drive, and Buildroot won't build in a path with spaces).
#
# The stick keeps the kernel version's layout (the Makefile's): the ramdisk
# (apps, wallpapers) at sector 2048 and the FAT32 partition at 16384, so
# lkx-shell opens the stick just as it opens build/litekernx.img on the PC.
# What changes: the boot code. SYSLINUX's MBR starts the FAT32 partition's
# SYSLINUX, which loads Linux and the system (an initramfs, run from RAM)
# from its /syslinux folder.
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
# LiteKern Go's output (x86_64, for the VM) is its own.
WORK=$HOME/lkx-buildroot
BR=$WORK/buildroot-$BR_VERSION
OUT=$WORK/output-go-x86_64
SYSLINUX=$WORK/syslinux-$SYSLINUX_VERSION
IMAGE=build-linux/litekern-go.img
FAT_OFFSET=$((16384 * 512))

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
if [ ! -f "$OUT/.config" ] || [ linux/buildroot.defconfig -nt "$OUT/.config" ]; then
    make -s -C "$BR" O="$OUT" BR2_EXTERNAL="$REPO/linux" BR2_DEFCONFIG="$REPO/linux/buildroot.defconfig" defconfig
fi

echo "== the toolchain"
make -s -C "$BR" O="$OUT" toolchain
[ "$step" = toolchain ] && exit 0

# The host side first: the disk image (its ramdisk and FAT32) and the files
# generated for lkx-shell (fonts, icons, cursors).
echo "== the host build (build/litekernx.img)"
make -s build/litekernx.img

# lkx-shell and the apps, with Buildroot's compiler, into the overlay.
echo "== lkx-shell and the apps, for the VM (x86_64)"
CROSS=$OUT/host/bin/x86_64-buildroot-linux-gnu-gcc
make -s -f platform/Makefile CC="$CROSS" ARCH_FLAGS= LBUILD=build-target KERNEL_BUILD=0
rm -rf "$OUT/lkx-overlay"
mkdir -p "$OUT/lkx-overlay/usr/bin" "$OUT/lkx-overlay/usr/lib/litekern/apps"
cp build-target/lkx-shell "$OUT/lkx-overlay/usr/bin/"
for app in apps/*/; do
    n=$(basename "$app")
    mkdir -p "$OUT/lkx-overlay/usr/lib/litekern/apps/$n"
    cp "build-target/apps/$n/$n" "$OUT/lkx-overlay/usr/lib/litekern/apps/$n/"
done

echo "== Linux and the system"
# Buildroot doesn't notice a changed kernel.config by itself.
if [ -f "$OUT/.lkx-kernel-config" ] && [ linux/kernel.config -nt "$OUT/.lkx-kernel-config" ]; then
    make -s -C "$BR" O="$OUT" linux-reconfigure
fi
touch "$OUT/.lkx-kernel-config"
if [ -f "$OUT/.lkx-busybox-config" ] && [ linux/busybox.fragment -nt "$OUT/.lkx-busybox-config" ]; then
    make -s -C "$BR" O="$OUT" busybox-reconfigure
fi
touch "$OUT/.lkx-busybox-config"
make -s -C "$BR" O="$OUT"

echo "== the image"
mkdir -p build-linux
cp build/litekernx.img "$IMAGE"
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
mcopy -o -i "$IMAGE@@$FAT_OFFSET" "$OUT/images/rootfs.cpio.gz" ::/syslinux/rootfs.gz
mcopy -o -i "$IMAGE@@$FAT_OFFSET" linux/syslinux.cfg ::/syslinux/syslinux.cfg
"$SYSLINUX/bios/mtools/syslinux" --offset "$FAT_OFFSET" --directory /syslinux --install "$IMAGE"
echo "== done: $IMAGE ($(du -h "$IMAGE" | cut -f1))"
