#!/usr/bin/env bash
# LiteKern X on Linux — VMs for the USB stick image (linux/build.sh), so it can
# be tried without the EeePC. In WSL, from the project folder:
#
#   bash linux/vm.sh qemu           a QEMU window, the EeePC's 1024x600 screen
#   bash linux/vm.sh qemu --fresh   the same, starting again from the image
#   (the VirtualBox VM "LiteKern Go": vm\create-vm.ps1, once, from PowerShell)
#   bash linux/vm.sh vbox start     boot it in a window
#   bash linux/vm.sh vbox test      boot it headless until the desktop is up
#   bash linux/vm.sh vbox stop | status | destroy
#
# Each VM keeps its own copy of the disk, so settings, users and files stay
# between runs; a newer image (a new linux/build.sh) replaces it. The log
# goes to the VM's serial port: the QEMU window's terminal, or
# build-linux/vbox/serial.log.
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=build-linux/litekernx-linux.img
[ -f "$IMAGE" ] || { echo "$IMAGE missing: run linux/build.sh first" >&2; exit 1; }

case "${1:-}" in
qemu)
    disk=build-linux/vm/qemu.img
    mkdir -p build-linux/vm
    if [ "${2:-}" = --fresh ] || [ ! -f "$disk" ] || [ "$IMAGE" -nt "$disk" ]; then
        cp "$IMAGE" "$disk"
        echo "the VM's disk: a fresh copy of $IMAGE"
    fi
    # Hardware virtualization when WSL lets us (KVM: many times faster than
    # QEMU emulating the CPU). It needs this user in the kvm group:
    #   wsl -u root usermod -aG kvm $USER   then   wsl --shutdown
    accel=()
    if [ -w /dev/kvm ]; then
        accel=(-accel kvm)
        echo "QEMU: KVM (hardware virtualization)"
    else
        echo "QEMU: no KVM, so the CPU is emulated: slow. To fix, from PowerShell:"
        echo "  wsl -u root usermod -aG kvm $USER"
        echo "  wsl --shutdown"
    fi
    # virtio-vga, offering the EeePC panel's 1024x600 first: its hardware
    # cursor is LiteKern X's pointer, which QEMU's window shows as the PC's
    # own (drawn by Windows: no lag through WSL's display). A USB tablet: the
    # pointer goes where the PC's is, with no grabbing the mouse.
    exec bash vm/qemu.sh --image "$disk" -- -vga none -device virtio-vga,xres=1024,yres=600 \
        -usb -device usb-tablet "${accel[@]}"
    ;;
vbox)
    shift
    # LiteKern Go's VM, sized like the phone, is made by vm/create-vm.ps1
    # (SATA: the image on port 0, the 16 GB disk on port 2).
    if [ "${1:-}" = create ]; then
        echo "From PowerShell: powershell -ExecutionPolicy Bypass -File vm\\create-vm.ps1" >&2
        exit 1
    fi
    export VM="LiteKern Go" DISK_UUID=4c4b5800-0000-4000-8000-000000000003 STORAGECTL=SATA
    export KEEP_DISK=1 IMAGE DIR=build-linux/vbox MOUSE=usbtablet
    export WAIT_FOR="${WAIT_FOR:-^desktop: ready}"
    exec bash vm/vbox.sh "$@"
    ;;
*)
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
