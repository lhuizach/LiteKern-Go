#!/usr/bin/env bash
# LiteKern Go: builds lkx-gld (phone/sandbox/gld.c, the 3D renderer for apps) inside the phone's Alpine sandbox and
# leaves it at /extras/alpine/usr/local/bin/lkx-gld. Run in WSL with the phone running and plugged in, /extras mounted,
# and mesa-dev, gcc, musl-dev in the sandbox (the browser's build tools have them).
set -euo pipefail
cd "$(dirname "$0")"
KEY=${LKX_PHONE_KEY:-$HOME/.ssh/litekern-phone}
SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 "${LKX_PHONE:-root@172.16.42.1}")
"${SSH[@]}" 'cat > /extras/alpine/root/gld.c' < gld.c
"${SSH[@]}" '/extras/enter.sh sh -c "mkdir -p /usr/local/bin; cd /root && gcc -O2 -Wall -o /usr/local/bin/lkx-gld gld.c \$(pkg-config --cflags --libs egl glesv2 gbm) -lm && ls -l /usr/local/bin/lkx-gld"' < /dev/null
