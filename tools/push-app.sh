#!/usr/bin/env bash
# LiteKern Go: builds an app for the phone and puts it there, over the USB network, to try it
# right away. Run in WSL, with the phone running and plugged in (172.16.42.1).
#
#   bash tools/push-app.sh NAME            a changed app, or a new one
#   bash tools/push-app.sh NAME --no-restart   don't restart the desktop even if the app is new
#
# What it does: cross-builds the apps for the phone and copies NAME's program into the phone's
# /usr/lib/litekern/apps/NAME/, stopping a running copy so the next start is the new one.
# An app that's in the system already (its icon is there) needs nothing more. A NEW app isn't in the
# desktop's list yet: its kerns.json goes next to its program (the desktop also reads those, as it
# does for the Web app) and the desktop is restarted to read it; open apps are closed by that.
# Your files and settings (the data disk) aren't touched.
#
# It lasts until the phone restarts: to keep an app, rebuild the system and put it on the phone
# (tools/build-system.sh, then phone/update-system.sh, which needs the owner's yes in chat).
set -euo pipefail
cd "$(dirname "$0")/.."
name=${1:-}
flag=${2:-}
[ -n "$name" ] && [ -f "apps/$name/kerns.json" ] || { echo "usage: bash tools/push-app.sh NAME [--no-restart] (a folder in apps/)" >&2; exit 2; }
KEY=${LKX_PHONE_KEY:-$HOME/.ssh/litekern-phone}
PHONE=${LKX_PHONE:-root@172.16.42.1}
BR=${LKX_BUILDROOT:-$HOME/lkx-buildroot/output-go-phone}
SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 "$PHONE")
title=$(python3 -c "import json; print(json.load(open('apps/$name/kerns.json', encoding='utf-8'))['name'])")

"${SSH[@]}" 'true' || { echo "the phone doesn't answer over ssh (running? plugged in?)" >&2; exit 1; }

echo "== build for the phone"
CLEAN_PATH=$(printf '%s' "$PATH" | tr ':' '\n' | grep -v ' ' | grep -v '^/mnt/' | paste -sd: -)
PATH=$CLEAN_PATH make -s -f platform/Makefile CC="$BR/host/bin/aarch64-buildroot-linux-gnu-gcc" ARCH_FLAGS= OPT=-O3 \
    LBUILD=build-target-phone KERNEL_BUILD=0
bin=build-target-phone/apps/$name/$name
[ -f "$bin" ] || { echo "no $bin: the build didn't make the app" >&2; exit 1; }

echo "== copy to the phone"
dest=/usr/lib/litekern/apps/$name
"${SSH[@]}" "mkdir -p $dest; killall $name 2>/dev/null; true"
"${SSH[@]}" "cat > $dest/$name.new" < "$bin"
"${SSH[@]}" "chmod 755 $dest/$name.new; mv -f $dest/$name.new $dest/$name"
known=$("${SSH[@]}" "grep -c '^lkx: $title ' /tmp/lkx.log || true")
if [ "${known:-0}" -ge 1 ]; then
    echo "== the desktop already lists $title: open it from the home screen"
else
    # the manifest only for an app the system doesn't have: one that's in it already would show twice
    "${SSH[@]}" "cat > $dest/kerns.json" < "apps/$name/kerns.json"
    if [ "$flag" = "--no-restart" ]; then
        echo "== $title is new to the desktop: it'll show after the desktop restarts (not done: --no-restart)"
    else
        echo "== $title is new: restarting the desktop so it reads the new list of apps"
        "${SSH[@]}" 'killall lkx-shell' || true               # init starts it again (lkx-start)
        echo "   (the desktop is starting again; give it a few seconds)"
    fi
fi
echo "== done: $title is on the phone until it restarts"
