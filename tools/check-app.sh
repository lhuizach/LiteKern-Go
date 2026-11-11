#!/usr/bin/env bash
# LiteKern Go: a quick check of one app, in about a minute (the full tests take ten).
#
#   bash tools/check-app.sh NAME
#
# Builds everything for the PC with the project's strict flags (-Wall -Wextra -Werror), then runs
# the desktop headless at the phone's size, taps the app's icon, goes home, and fails if the app
# doesn't open or if anything crashed. Needs WSL (like the tests). Writes build-linux/check/.
set -euo pipefail
cd "$(dirname "$0")/.."
name=${1:-}
[ -n "$name" ] && [ -f "apps/$name/kerns.json" ] || { echo "usage: bash tools/check-app.sh NAME (a folder in apps/)" >&2; exit 2; }
title=$(python3 -c "import json,sys; print(json.load(open('apps/$name/kerns.json', encoding='utf-8'))['name'])")

echo "== build"
make -s -f platform/Makefile > /dev/null
make -s > /dev/null                      # the disk image the desktop reads (its ramdisk lists the apps)
mkdir -p build-linux/check
cp build/litekernx.img build-linux/check/disk.img

echo "== open $title"
read -r x y < <(python3 tests/linux/appslot.py "$title")
steps="move:$((x - 360)),$((y - 640)) sleep:0.2 press:1 release sleep:2 phone_home sleep:1"
out=$(timeout 60 build-linux/lkx-shell --backend headless --disk build-linux/check/disk.img --size 720x1280 \
        --keys "$steps" --for 12 2>&1 || true)
fail=0
grep -q "^desktop: open $title\$" <<<"$out" && echo "PASS  it opened" || { echo "FAIL  it didn't open (looked at $x,$y)"; fail=1; }
if grep -qE "crashed|stopped|PANIC|Segmentation" <<<"$out"; then
    echo "FAIL  something crashed:"; grep -E "crashed|stopped|PANIC|Segmentation" <<<"$out" | head -3; fail=1
else
    echo "PASS  nothing crashed"
fi
grep -E "^user: $name:" <<<"$out" | head -5 | sed 's/^/      /'
[ "$fail" = 0 ] && echo "check: ok" || { echo "check: FAILED"; exit 1; }
