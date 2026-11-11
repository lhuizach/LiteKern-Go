#!/usr/bin/env bash
# LiteKern Go: builds the phone's system (all the apps, the shell, the drivers' modules, in the
# initramfs) so a change can be put on the phone for good. Run in WSL.
#
#   bash tools/build-system.sh                  just the system
#   bash tools/build-system.sh --tests          the full tests first (about ten minutes)
#   bash tools/build-system.sh --kernel         also the kernel and device tree (phone/build-kernel.sh)
#   bash tools/build-system.sh --ramboot NAME   also a boot image to try from RAM with `fastboot boot`:
#                                               "Apps for zach/to-fedora/boot-litekern-NAME.img"
#
# Then, to put it on the phone (this needs the owner's yes in chat, every time):
#   bash phone/update-system.sh --check
#   bash phone/update-system.sh --write system      (kernel, dtb, extlinux: see that script)
# This makes files only. It flashes and writes nothing to the phone.
set -euo pipefail
cd "$(dirname "$0")/.."
tests=0 kernel=0 ramboot=
while [ $# -gt 0 ]; do
    case "$1" in
    --tests) tests=1 ;;
    --kernel) kernel=1 ;;
    --ramboot) shift; ramboot=${1:?--ramboot needs a name, like go37} ;;
    *) sed -n '3,14p' "$0"; exit 2 ;;
    esac
    shift
done
if [ "$tests" = 1 ]; then
    echo "== the tests"
    LKX_ARCH=x86_64 bash tests/linux/test-linux.sh | tail -3
fi
if [ "$kernel" = 1 ]; then
    echo "== the kernel"
    bash phone/build-kernel.sh > /tmp/build-kernel.log 2>&1 || { tail -20 /tmp/build-kernel.log; exit 1; }
fi
echo "== the system"
LKX_ARCH=aarch64 LKX_PHONE=1 bash linux/build.sh > /tmp/build-system.log 2>&1 || { tail -20 /tmp/build-system.log; exit 1; }
cpio=$HOME/lkx-buildroot/output-go-phone/images/rootfs.cpio.xz
printf 'system: %s bytes, sha256 %s\n' "$(stat -c %s "$cpio")" "$(sha256sum "$cpio" | cut -c1-64)"
if [ -n "$ramboot" ]; then
    echo "== a RAM boot image: $ramboot"
    bash phone/make-ramboot.sh > /tmp/make-ramboot.log 2>&1 || { tail -20 /tmp/make-ramboot.log; exit 1; }
    out="Apps for zach/to-fedora/boot-litekern-$ramboot.img"
    cp "$HOME/lkx-phone/out/boot-litekern-go.img" "$out"
    printf '%s: sha256 %s\n' "$out" "$(sha256sum "$out" | cut -c1-64)"
fi
echo "next: bash phone/update-system.sh --check, and (with the owner's yes) --write system"
