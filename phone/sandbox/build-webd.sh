#!/usr/bin/env bash
# LiteKern Go: builds lkx-webd (phone/sandbox/webd.c) inside the phone's Alpine sandbox and leaves it at
# /extras/alpine/usr/local/bin/lkx-webd. Run in WSL with the phone running and plugged in, /extras mounted,
# and the build tools in the sandbox (apk add build-base pkgconf wpewebkit-dev libwpe-dev
# libwpebackend-fdo-dev glib-dev wayland-dev, inside /extras/enter.sh).
set -euo pipefail
cd "$(dirname "$0")"
KEY=${LKX_PHONE_KEY:-$HOME/.ssh/litekern-phone}
SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 "${LKX_PHONE:-root@172.16.42.1}")
"${SSH[@]}" 'cat > /extras/alpine/root/webd.c' < webd.c
"${SSH[@]}" '/extras/enter.sh sh -c "mkdir -p /usr/local/bin; cd /root && gcc -O2 -Wall -o /usr/local/bin/lkx-webd webd.c \$(pkg-config --cflags --libs wpe-webkit-2.0 wpebackend-fdo-1.0 wayland-server glib-2.0) && ls -l /usr/local/bin/lkx-webd"'
