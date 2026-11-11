#!/usr/bin/env bash
# LiteKern Go, phone: the image for the phone's userdata partition, so our system boots from
# storage instead of RAM and keeps its settings, Wi-Fi, PIN and photos across restarts.
#
#   LKX_ARCH=aarch64 LKX_PHONE=1 bash linux/build.sh     (the system: rootfs.cpio.xz)
#   bash phone/build-kernel.sh                           (the kernel and its device tree)
#   bash phone/make-userdata.sh
#
# Out (~/lkx-phone/out/): userdata-litekern.sparse.img, what `fastboot flash userdata` takes
# (and userdata-litekern.raw, what it holds, for the checks in phone/verify-userdata.sh).
# This only makes files. Flashing is a separate step, only with the owner's confirmation
# each time (CLAUDE.md). Layout: phone/mkuserdata.py.
#
# The boot partition is made like Nura's (ext2, 1 KiB blocks, extlinux/extlinux.conf with
# kernel, fdt and initrd lines): lk2nd reads that.
set -euo pipefail
cd "$(dirname "$0")/.."
W=$HOME/lkx-phone
O=$W/out
CPIO=${1:-$HOME/lkx-buildroot/output-go-phone/images/rootfs.cpio.xz}
DATA_MB=${DATA_MB:-2048}                # the data disk's FAT32, in MiB
T=$W/userdata-build
for f in "$O/Image.gz" "$O/msm8916-samsung-a5u-eur.dtb" "$CPIO"; do
    [ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
rm -rf "$T" && mkdir -p "$T/boot/extlinux"

# 1. the data disk: the shell's own disk image, with a big FAT32 (4 KiB clusters)
make -s BUILD=build/userdata FAT_MB="$DATA_MB" FAT_SPC=8 build/userdata/litekernx.img
cp build/userdata/litekernx.img "$T/data.img"

# 2. the boot partition. The kernel is the plain Image.gz: the device tree is its own file.
cp "$O/Image.gz" "$T/boot/vmlinuz"
cp "$O/msm8916-samsung-a5u-eur.dtb" "$T/boot/"
cp "$CPIO" "$T/boot/initramfs"
cat > "$T/boot/extlinux/extlinux.conf" <<'EOF'
timeout 1
default litekern

label litekern
	kernel /vmlinuz
	fdt /msm8916-samsung-a5u-eur.dtb
	initrd /initramfs
	append console=tty0 console=ttyMSM0,115200 loglevel=5
EOF
rm -f "$T/boot.ext2"
mke2fs -q -t ext2 -b 1024 -I 256 -L LKGO_BOOT -E lazy_itable_init=0,root_owner=0:0 -d "$T/boot" "$T/boot.ext2" 64M

# 3. the whole image, and its sparse form
python3 phone/mkuserdata.py "$T/boot.ext2" "$T/data.img" "$O/userdata-litekern.raw"
python3 phone/sparse.py pack "$O/userdata-litekern.raw" "$O/userdata-litekern.sparse.img"
sha256sum "$O/userdata-litekern.sparse.img" | cut -c1-64
ls -la "$O/userdata-litekern.sparse.img" | awk '{print $5, "bytes (sparse)"}'
