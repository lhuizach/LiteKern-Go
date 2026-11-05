#!/usr/bin/env bash
# LiteKern Go — installs the toolchain and QEMU in WSL Ubuntu.
#
# Usage (from the project folder): sudo bash tools/setup-wsl.sh
set -euo pipefail

pkgs=(
    build-essential   # gcc, ld, make
    gdb               # debugging against QEMU's gdb stub
    qemu-system-x86   # qemu-system-x86_64
    qemu-system-gui   # GTK window (shown on Windows through WSLg)
)

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo bash tools/setup-wsl.sh" >&2
    exit 1
fi

apt-get update
apt-get install -y "${pkgs[@]}"
echo "Done. Check with: make check-tools"
