#!/usr/bin/env bash
# LiteKern Go, phase 6: the phone's kernel, the same source, patch and config
# as Nura's samsung-a5 (phone/fetch.sh), built with our Buildroot compiler for
# the Cortex-A53. pmbootstrap would do this in a chroot, which needs sudo;
# this needs no root. Run in WSL:
#
#   LKX_ARCH=aarch64 bash linux/build.sh toolchain     (once)
#   bash phone/build-kernel.sh
#
# Out: ~/lkx-phone/out/{Image.gz,msm8916-samsung-a5u-eur.dtb,modules/}
# Nothing here flashes anything.
set -euo pipefail
cd "$(dirname "$0")"
PHONE=$(pwd)
VER=$(sed -n 's/^pkgver=//p' kernel/APKBUILD)
TAG=v$VER-msm8916
W=$HOME/lkx-phone
SRC=$W/linux-$VER-msm8916
OUT=$W/out
BUILD=$W/build-kernel
CROSS=$HOME/lkx-buildroot/output-go-aarch64/host/bin/aarch64-buildroot-linux-gnu-
[ -x "${CROSS}gcc" ] || { echo "no ${CROSS}gcc: run LKX_ARCH=aarch64 bash linux/build.sh toolchain" >&2; exit 1; }
PATH=$(printf '%s' "$PATH" | tr ':' '\n' | grep -v ' ' | grep -v '^/mnt/' | paste -sd: -)
HOST=$HOME/lkx-buildroot/output-go-aarch64/host     # its flex, bison, libssl, libelf
PATH=$HOME/lkx-buildroot/gnubin:$HOST/bin:$PATH
export HOSTCFLAGS="-I$HOST/include" HOSTLDFLAGS="-L$HOST/lib -Wl,-rpath,$HOST/lib"
mkdir -p "$W/dl" "$BUILD" "$OUT"
TAR=$W/dl/linux-$TAG.tar.gz
if [ ! -d "$SRC" ]; then
    if [ ! -s "$TAR" ]; then
        wget -nv -O "$TAR.part" "https://github.com/msm8916-mainline/linux/archive/$TAG.tar.gz"
        mv "$TAR.part" "$TAR"
    fi
    tar -xf "$TAR" -C "$W"
    for p in kernel/*.patch; do patch -s -d "$SRC" -p1 < "$PHONE/$p"; done
fi
M="make -s -C $SRC O=$BUILD ARCH=arm64 CROSS_COMPILE=$CROSS LOCALVERSION= -j$(nproc)"
# The config inside the Nura v26.06 kernel we flash first (read from its image,
# CONFIG_IKCONFIG), not pmaports' newest: that one differs (evdev a module...)
# and wouldn't match the v26.06 rootfs.
cp kernel/config-nura-v26.06.aarch64 "$BUILD/.config"
$M olddefconfig
$M Image.gz dtbs modules
cp "$BUILD/arch/arm64/boot/Image.gz" "$OUT/"
cp "$BUILD/arch/arm64/boot/dts/qcom/msm8916-samsung-a5u-eur.dtb" "$OUT/"
rm -rf "$OUT/modules"
$M INSTALL_MOD_PATH="$OUT/modules" INSTALL_MOD_STRIP=1 modules_install
echo "== done: $(make -s -C "$SRC" O="$BUILD" ARCH=arm64 kernelrelease)"
ls -la "$OUT"
