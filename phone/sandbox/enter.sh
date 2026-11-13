#!/bin/sh
# LiteKern Go, phone: runs a command inside the Alpine sandbox in the extras area (docs/BROWSER-PLAN.md).
# Lives on the phone as /extras/enter.sh.   enter.sh CMD [ARGS]...   (no command: a shell)
R=/extras/alpine
mountpoint -q $R/proc || mount -t proc proc $R/proc
mountpoint -q $R/dev || mount --rbind /dev $R/dev
mountpoint -q $R/sys || mount --rbind /sys $R/sys
cp /etc/resolv.conf $R/etc/resolv.conf 2>/dev/null
[ $# -gt 0 ] || set -- /bin/sh
exec chroot $R "$@"
