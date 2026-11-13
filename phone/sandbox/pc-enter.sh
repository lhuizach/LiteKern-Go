#!/bin/sh
# LiteKern Go: runs a command inside the x86_64 Alpine test sandbox on the PC (WSL, no admin rights:
# a user namespace). The sandbox is ~/lkx-web-p0/root, see docs/BROWSER-PLAN.md.   pc-enter.sh CMD [ARGS]...
d=${LKX_P0:-$HOME/lkx-web-p0}
exec unshare -r -m -p -f --mount-proc="$d/root/proc" sh -c 'd=$1; shift; mount --rbind /dev "$d/root/dev"; cp /etc/resolv.conf "$d/root/etc/"; exec chroot "$d/root" "$@"' sh "$d" "$@"
