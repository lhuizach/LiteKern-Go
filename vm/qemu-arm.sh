#!/usr/bin/env bash
# LiteKern Go — the ARM build (phase 4) in qemu-system-aarch64: QEMU's virt
# machine with the phone's CPU (a Cortex-A53, four of them), 2 GB, and the
# phone's 720x1280 screen (virtio-gpu). Buildroot builds this QEMU (WSL's own
# would need sudo: linux/buildroot-aarch64.defconfig).
#
#   bash vm/qemu-arm.sh [--headless] [--mouse] --image DISK [-- MORE QEMU ARGS]
#
# The kernel (build-linux/aarch64/Image, the system built in) is given
# straight to QEMU; DISK is the stick (its ramdisk and FAT32, lkx-shell's).
# A window, or with --headless none: the serial port (the log) on stdout.
# Input: virtio's keyboard, and its tablet (the pointer where the PC's is) or
# with --mouse its relative mouse (what the tests' mouse_move needs).
# No KVM for ARM on a PC: the CPU is emulated, so it's slower than the VM.
set -euo pipefail
QEMU=${QEMU_ARM:-$HOME/lkx-buildroot/output-go-aarch64/host/bin/qemu-system-aarch64}
KERNEL=${KERNEL:-build-linux/aarch64/Image}
APPEND="console=ttyAMA0,115200 quiet loglevel=3 video=720x1280 vt.global_cursor_default=0 ${EXTRA_APPEND:-}"

image= display=() pointer=(-device virtio-tablet-pci) extra=()
while [ $# -gt 0 ]; do
    case "$1" in
        --image)    image=$2; shift 2 ;;
        --headless) display=(-display none); shift ;;
        --mouse)    pointer=(-device virtio-mouse-pci); shift ;;
        --)         shift; extra=("$@"); break ;;
        *)          echo "usage: vm/qemu-arm.sh [--headless] [--mouse] --image DISK [-- QEMU ARGS]" >&2; exit 2 ;;
    esac
done
[ -n "$image" ] && [ -f "$image" ] || { echo "vm/qemu-arm.sh: no disk image ($image)" >&2; exit 1; }
[ -x "$QEMU" ] || { echo "vm/qemu-arm.sh: $QEMU missing: run LKX_ARCH=aarch64 bash linux/build.sh" >&2; exit 1; }
[ -f "$KERNEL" ] || { echo "vm/qemu-arm.sh: $KERNEL missing: run LKX_ARCH=aarch64 bash linux/build.sh" >&2; exit 1; }

exec "$QEMU" -name "LiteKern Go (ARM)" -M virt -cpu cortex-a53 -smp 4 -m 2G \
    -kernel "$KERNEL" -append "$APPEND" \
    -drive "file=$image,format=raw,if=none,id=disk" -device virtio-blk-pci,drive=disk \
    -device virtio-gpu-pci,xres=720,yres=1280 -device virtio-keyboard-pci "${pointer[@]}" \
    -netdev user,id=net -device virtio-net-pci,netdev=net \
    -serial stdio -no-reboot "${display[@]}" "${extra[@]}"
