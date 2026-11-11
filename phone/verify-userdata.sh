#!/usr/bin/env bash
# LiteKern Go, phone: checks the userdata image from phone/make-userdata.sh without a phone.
#
#   bash phone/verify-userdata.sh
#
# The sparse file unpacks to exactly the raw image; the partition table is what the phone's
# start-up and lk2nd expect; the boot partition's filesystem is clean and holds the very kernel,
# device tree and system that went in; the data disk has its marker, its ramdisk and a clean
# FAT32. Reads files only: it flashes nothing.
set -euo pipefail
cd "$(dirname "$0")/.."
O=$HOME/lkx-phone/out
RAW=$O/userdata-litekern.raw
SPARSE=$O/userdata-litekern.sparse.img
CPIO=${1:-$HOME/lkx-buildroot/output-go-phone/images/rootfs.cpio.xz}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
ok() { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; failed=1; }
failed=0

# 1. the sparse file is the raw image
python3 phone/sparse.py unpack "$SPARSE" "$T/unpacked.raw" > /dev/null
if cmp -s "$T/unpacked.raw" "$RAW"; then ok "the sparse file unpacks to exactly the raw image ($(stat -c %s "$RAW") bytes)"; else bad "the sparse file does not unpack to the raw image"; fi
python3 - "$SPARSE" <<'PY' && ok "only RAW and FILL chunks (what lk2nd is known to take)" || bad "other chunk types in the sparse file"
import struct, sys
f = open(sys.argv[1], "rb")
h = struct.unpack("<IHHHHIIII", f.read(28))
f.seek(h[3])
types = set()
for _ in range(h[7]):
    t, _r, csz, tsz = struct.unpack("<HHII", f.read(12))
    types.add(t)
    f.seek(tsz - 12, 1)
sys.exit(0 if types <= {0xCAC1, 0xCAC2} else 1)
PY

# 2. the partition table
fdisk -l "$RAW" 2>/dev/null | grep -E "^$RAW" | sed "s#$RAW#image#" | sed 's/^/      /'
p1=$(fdisk -l "$RAW" 2>/dev/null | awk -v r="$RAW" '$1==r"1" {print $3, $5}')
set -- $p1
[ "$1" = "*" ] || true
start1=$(fdisk -l "$RAW" 2>/dev/null | awk -v r="$RAW" '$1==r"1" {print ($2=="*")?$3:$2}')
start2=$(fdisk -l "$RAW" 2>/dev/null | awk -v r="$RAW" '$1==r"2" {print $2}')
[ "$start1" = 2048 ] && [ "$start2" = 133120 ] && ok "partition 1 at 2048, partition 2 (the data disk) at 133120, as lkx-start expects" || bad "partition starts $start1 / $start2"

# 3. the boot partition
dd if="$RAW" of="$T/boot.ext2" bs=512 skip=2048 count=131072 status=none
if e2fsck -fn "$T/boot.ext2" > "$T/fsck.txt" 2>&1; then ok "the boot partition's ext2 is clean"; else bad "ext2 problems: $(tail -3 "$T/fsck.txt")"; fi
debugfs -R "ls -l /" "$T/boot.ext2" 2>/dev/null | awk 'NF>=7 {print "      " $7, $6}' | head -8
debugfs -R "cat /extlinux/extlinux.conf" "$T/boot.ext2" 2>/dev/null > "$T/conf.txt"
grep -q "^	kernel /vmlinuz" "$T/conf.txt" && grep -q "^	fdt /msm8916-samsung-a5u-eur.dtb" "$T/conf.txt" && grep -q "^	initrd /initramfs" "$T/conf.txt" && ok "extlinux.conf has the kernel, fdt and initrd lines" || bad "extlinux.conf is wrong"
for pair in "vmlinuz:$O/Image.gz" "msm8916-samsung-a5u-eur.dtb:$O/msm8916-samsung-a5u-eur.dtb" "initramfs:$CPIO"; do
    n=${pair%%:*}; src=${pair#*:}
    debugfs -R "dump /$n $T/$n" "$T/boot.ext2" 2>/dev/null
    if cmp -s "$T/$n" "$src"; then ok "/$n is the same file as $(basename "$src")"; else bad "/$n differs from $src"; fi
done

# 4. the data disk
dd if="$RAW" of="$T/data.head" bs=512 skip=133120 count=1 status=none
[ "$(head -c 8 "$T/data.head")" = LKGODATA ] && ok "the data disk starts with its LKGODATA mark" || bad "no LKGODATA mark"
dd if="$RAW" bs=512 skip=$((133120 + 4096)) count=1 status=none | head -c 4 | grep -q LKXR && ok "its ramdisk mark (LKXR) is at sector 4096" || bad "no ramdisk mark"
fat_start=$(python3 - "$RAW" <<'PY'
import struct, sys
f = open(sys.argv[1], "rb")
f.seek(133120 * 512 + 446 + 16)
e = f.read(16)
print(struct.unpack("<II", e[8:16])[0], struct.unpack("<II", e[8:16])[1])
PY
)
set -- $fat_start
dd if="$RAW" of="$T/fat.img" bs=512 skip=$((133120 + $1)) count="$2" status=none
if fsck.fat -n "$T/fat.img" > "$T/fat.txt" 2>&1; then ok "the data disk's FAT32 is clean ($(grep -o '[0-9]* clusters' "$T/fat.txt" | head -1), $((512 * $2 / 1048576)) MiB)"; else bad "FAT32 problems: $(tail -3 "$T/fat.txt")"; fi
echo
[ "$failed" = 0 ] && echo "verify: all checks passed" || { echo "verify: FAILED"; exit 1; }
