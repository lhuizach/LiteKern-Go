#!/usr/bin/env bash
# One scripted run of R3lay against tests/r3lay/mock_supabase.py, for working on
# it by eye: bash tests/r3lay/run-mock.sh OUTDIR "STEPS"
# STEPS is the shell's key language (platform/headless.c).
set -uo pipefail
cd "$(dirname "$0")/../.."
OUT=$1 STEPS=$2 PORT=${PORT:-18741}
mkdir -p "$OUT"
rm -f "$OUT/mock.log"
python3 tests/r3lay/mock_supabase.py "$PORT" "$OUT/mock.log" &
MOCK=$!
trap 'kill $MOCK 2>/dev/null' EXIT
sleep 0.5
printf 'url=http://127.0.0.1:%s\nkey=test-public-key\n' "$PORT" > "$OUT/r3lay.conf"
cp build/litekernx.img "$OUT/disk.img"
R3LAY_CONF=$PWD/$OUT/r3lay.conf timeout "${LIMIT:-90}" build-linux/lkx-shell --backend headless --disk "$OUT/disk.img" \
    --size 720x1280 --for "${FOR:-80}" --keys "$STEPS" --shot "$OUT/last.png" > "$OUT/shell.log" 2>&1 < /dev/null
echo "exit $?"
grep -E 'r3lay|crash|stopped|Segmentation' "$OUT/shell.log" | head -40
cat "$OUT/mock.log"
