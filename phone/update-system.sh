#!/usr/bin/env bash
# LiteKern Go, phone: puts new system files on a phone that boots from its own storage
# (phone/make-userdata.sh), over the USB network, without touching the user's data.
#
#   bash phone/update-system.sh --check                 look only: what's on the phone, what would change
#   bash phone/update-system.sh --write extlinux        just the boot config (phone/extlinux.conf)
#   bash phone/update-system.sh --write system          the system (the initramfs: apps and all)
#   bash phone/update-system.sh --write dtb             the device tree
#   bash phone/update-system.sh --write kernel          the kernel
#   bash phone/update-system.sh --write all             all four (the config last)
#
# What it writes: only files on the boot partition, the first partition inside userdata (an ext2
# filesystem, label LKGO_BOOT, 64 MiB from 1 MiB in). The data disk (the user's settings, Wi-Fi,
# PIN, photos) starts right after it and is never opened. Each file goes down as NAME.new, is
# checked against its checksum here, and only then renamed over the old one, so a file is never
# half there. The config goes last, after the files it names.
#
# Every --write is done only with the owner's confirmation in chat (CLAUDE.md).
#
# If an update ever leaves the phone not booting: power off, power on holding Volume Down (lk2nd),
# `fastboot boot` a known good image (Apps for zach/to-fedora/boot-litekern-goN.img), and run this
# again from that: the boot partition is the same, and the data was never touched.
#
# Runs in WSL, which reaches the phone over the USB network (172.16.42.1). LKX_PHONE_KEY is the ssh key
# (default ~/.ssh/litekern-phone). Needs the phone running our system.
set -euo pipefail
cd "$(dirname "$0")/.."
KEY=${LKX_PHONE_KEY:-$HOME/.ssh/litekern-phone}
PHONE=${LKX_PHONE:-root@172.16.42.1}
OUT=${LKX_OUT:-$HOME/lkx-phone/out}
SYSTEM=${LKX_SYSTEM:-$HOME/lkx-buildroot/output-go-phone/images/rootfs.cpio.xz}
BOOT_OFFSET=$((2048 * 512))

mode=${1:-}
what=${2:-}
case "$mode" in
--check) ;;
--write) [ -n "$what" ] || { sed -n '3,10p' "$0"; exit 2; } ;;
*) sed -n '3,10p' "$0"; exit 2 ;;
esac

SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 "$PHONE")

# the name on the phone, then the file here
declare -A SRC=(
    [vmlinuz]="$OUT/Image.gz"
    [msm8916-samsung-a5u-eur.dtb]="$OUT/msm8916-samsung-a5u-eur.dtb"
    [initramfs]="$SYSTEM"
    [extlinux/extlinux.conf]="phone/extlinux.conf"
)
ALL=(vmlinuz msm8916-samsung-a5u-eur.dtb initramfs extlinux/extlinux.conf)
case "$what" in
extlinux) list=(extlinux/extlinux.conf) ;;
system) list=(initramfs) ;;
dtb) list=(msm8916-samsung-a5u-eur.dtb) ;;
kernel) list=(vmlinuz) ;;
all | "") list=("${ALL[@]}") ;;
*) echo "unknown: $what" >&2; exit 2 ;;
esac
for n in "${list[@]}"; do
    [ -f "${SRC[$n]}" ] || { echo "missing: ${SRC[$n]}" >&2; exit 1; }
done

echo "== the phone"
"${SSH[@]}" 'echo "up $(cut -d. -f1 /proc/uptime) s; $(cut -c1-90 /proc/cmdline)"' || { echo "the phone doesn't answer over ssh" >&2; exit 1; }

# The script the phone runs to get at the boot partition: finds userdata, checks the first
# partition is ours (the label), and mounts it ($1: mount options, $2: losetup's; "-o ro" and "-r" to look
# only). The loop device runs on to the end of userdata, but the ext2 inside only ever uses its own 64 MiB.
# Any check that fails stops everything; the trap lets go of the mount and the loop device.
remote_open() {
    printf 'BOOT_OFFSET=%s\nMOPT="%s"\nLOPT="%s"\n' "$BOOT_OFFSET" "$1" "$2"
    cat <<'EOF'
set -eu
mnt=/mnt/lkgo-boot
part=
for p in /sys/class/block/mmcblk*p*; do
    if grep -qi '^PARTNAME=userdata$' "$p/uevent" 2>/dev/null; then part=/dev/${p##*/}; fi
done
[ -n "$part" ] || { echo "no userdata partition" >&2; exit 1; }
loop=$(losetup -f)
losetup $LOPT -o "$BOOT_OFFSET" "$loop" "$part"
trap 'umount "$mnt" 2>/dev/null || true; losetup -d "$loop" 2>/dev/null || true' EXIT
label=$(blkid "$loop" | sed -n 's/.*LABEL="\([^"]*\)".*/\1/p')
[ "$label" = LKGO_BOOT ] || { echo "the boot partition isn't ours (label '$label')" >&2; exit 1; }
mkdir -p "$mnt"
mount -t ext2 $MOPT "$loop" "$mnt"
echo "boot partition: $part via $loop, label $label ($MOPT)"
df -k "$mnt" | tail -1 | awk '{print "space: " $4 " KiB free of " $2}'
EOF
}

if [ "$mode" = "--check" ]; then
    echo "== the boot partition (read only)"
    {
        remote_open "-o ro" "-r"
        cat <<'EOF'
for f in vmlinuz msm8916-samsung-a5u-eur.dtb initramfs extlinux/extlinux.conf; do
    printf '%s  %s\n' "$(sha256sum "$mnt/$f" | cut -c1-64)" "$f"
done
echo "-- extlinux.conf there:"
cat "$mnt/extlinux/extlinux.conf"
EOF
    } | "${SSH[@]}" 'sh -s'
    echo "== here"
    for n in "${ALL[@]}"; do
        f=${SRC[$n]}
        if [ -f "$f" ]; then printf '%s  %s\n' "$(sha256sum "$f" | cut -c1-64)" "$n"; else printf '%-64s  %s (no file here)\n' "-" "$n"; fi
    done
    echo "(a line that differs from the phone's is what a --write of it would change)"
    exit 0
fi

echo "== writing: ${list[*]}"
for n in "${list[@]}"; do
    here=$(sha256sum "${SRC[$n]}" | cut -c1-64)
    echo "-- $n: $(stat -c %s "${SRC[$n]}") bytes, sha256 ${here:0:16}..."
    # first into the phone's RAM, where it can be checked without touching the boot partition
    "${SSH[@]}" 'mkdir -p /tmp/lkgo-up && rm -f /tmp/lkgo-up/new && cat > /tmp/lkgo-up/new' < "${SRC[$n]}"
    {
        remote_open "" ""
        printf 'name=%s\nwant=%s\n' "$n" "$here"
        cat <<'EOF'
got=$(sha256sum /tmp/lkgo-up/new | cut -c1-64)
[ "$got" = "$want" ] || { echo "the copy in the phone's RAM differs ($got)" >&2; exit 1; }
rm -f "$mnt/$name.new"
cp /tmp/lkgo-up/new "$mnt/$name.new"
sync
got=$(sha256sum "$mnt/$name.new" | cut -c1-64)
[ "$got" = "$want" ] || { rm -f "$mnt/$name.new"; echo "the file on the boot partition differs ($got): left the old one" >&2; exit 1; }
mv -f "$mnt/$name.new" "$mnt/$name"
sync
rm -f /tmp/lkgo-up/new
echo "installed $name"
EOF
    } | "${SSH[@]}" 'sh -s'
done
echo "== done. The change is used at the next start of the phone."
