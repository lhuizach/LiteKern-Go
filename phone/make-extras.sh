#!/usr/bin/env bash
# LiteKern Go, phone: makes the image of the "extras" area, an empty ext4 filesystem (label
# LKGO_EXTRAS, 3 GiB) that goes into userdata after the data disk, for big things like the web
# browser's sandbox (docs/BROWSER-PLAN.md). Run in WSL. Makes a file only; writes nothing to the phone.
#
#   bash phone/make-extras.sh            -> $LKX_OUT/extras.ext4 (default ~/lkx-phone/out/extras.ext4)
#
# Put on the phone with: bash phone/update-system.sh --write extras (needs the owner's yes in chat).
# Where it goes: sector EXTRAS_START of userdata, right after the data disk (which starts at 133120 and
# is 2 GiB: it ends at sector 4343808). Keep EXTRAS_START in step with linux/overlay/usr/bin/lkx-start.
set -euo pipefail
PATH=$PATH:/sbin:/usr/sbin   # mke2fs lives there
OUT=${LKX_OUT:-$HOME/lkx-phone/out}
SIZE_MIB=3072
mkdir -p "$OUT"
img=$OUT/extras.ext4
rm -f "$img"
truncate -s ${SIZE_MIB}M "$img"
# no journal (less writing to the flash), 4 KiB blocks, everything written out (no lazy init: the area
# on the phone holds old data, so what the filesystem expects to be zeros has to really be written)
mke2fs -q -t ext4 -O ^has_journal -b 4096 -L LKGO_EXTRAS -m 1 -E lazy_itable_init=0,lazy_journal_init=0,root_owner=0:0 "$img"
e2fsck -fn "$img" > /dev/null
printf '%s: %s bytes, sha256 %s\n' "$img" "$(stat -c %s "$img")" "$(sha256sum "$img" | cut -c1-64)"
