#!/usr/bin/env bash
# LiteKern X — the picture decoders (apps/files/image.c), tested on Linux.
#
# Builds them for Linux with AddressSanitizer and decodes every picture in
# tests/image/fixtures/, comparing with what Pillow made of it
# (<name>.ref.png; tests/image/make-fixtures.py made both). PNG and BMP must
# match exactly; JPEG within a little (IDCTs round differently). Damaged and
# non-picture files must fail cleanly, without a sanitizer report.
#
# Usage: bash tests/image/test-image.sh   (or: make test-image)
set -uo pipefail
cd "$(dirname "$0")/../.."

mkdir -p build/test-image
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I. \
    apps/files/image.c kernel/inflate.c tests/image/image_host.c -o build/test-image/image_host || exit 1

ASAN_OPTIONS=detect_leaks=0 python3 - <<'EOF'
import os, subprocess, sys
sys.path.insert(0, "tools")
from lkx_png import read_rgba

fx = "tests/image/fixtures"
failures = 0

def check(ok, what):
    global failures
    print(("PASS  " if ok else "FAIL  ") + what)
    failures += not ok

for name in sorted(os.listdir(fx)):
    if name.endswith(".ref.png"):
        continue
    path = os.path.join(fx, name)
    raw = "build/test-image/" + name + ".rgba"
    run = subprocess.run(["build/test-image/image_host", path, raw], capture_output=True, text=True)
    out = run.stdout.strip()
    if run.returncode or "Sanitizer" in run.stderr or "runtime error" in run.stderr:
        check(False, f"{name}: crashed or a sanitizer report\n{run.stderr[-2000:]}")
        continue
    ref = path + ".ref.png"
    if not os.path.exists(ref):                 # damaged on purpose: no crash, and refused
        if name.startswith("cut-short"):        # (a cut-short JPEG shows what's there)
            check(True, f"{name}: no crash ({out})")
        else:
            check(out.startswith("error "), f"{name}: refused cleanly ({out})")
        continue
    if not out.startswith("ok "):
        check(False, f"{name}: {out}")
        continue
    w, h, rows = read_rgba(ref)
    size = out.split()[1]
    if size != f"{w}x{h}":
        check(False, f"{name}: {size}, expected {w}x{h}")
        continue
    got = open(raw, "rb").read()
    want = b"".join(bytes(p) for r in rows for p in r)
    diffs = [abs(a - b) for a, b in zip(got, want)]
    worst, mean = max(diffs), sum(diffs) / len(diffs)
    if name.endswith(".jpg"):
        ok = mean < 1.5 and worst <= 40
    else:
        # Fully transparent pixels: their colour doesn't matter.
        ok = all(d == 0 for i, d in enumerate(diffs) if want[i - i % 4 + 3] != 0 or i % 4 == 3)
    check(ok, f"{name}: {out[3:]}, mean difference {mean:.2f}, worst {worst}")

print()
print("image: all tests passed" if not failures else f"image: {failures} failed")
sys.exit(1 if failures else 0)
EOF
status=$?

# The compressor that saves a custom wallpaper (kernel/deflate.c): every
# input must come back the same through the kernel's inflate, and through
# Python's zlib (so it's real DEFLATE, not just self-consistent).
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I. \
    kernel/deflate.c kernel/inflate.c tests/image/deflate_host.c -o build/test-image/deflate_host || exit 1
mkdir -p build/test-image/deflate
ASAN_OPTIONS=detect_leaks=0 build/test-image/deflate_host build/test-image/deflate \
    build/test-image/baseline-420.jpg.rgba build/test-image/rgba.png.rgba || status=1
python3 - <<'PY' || status=1
import glob, sys, zlib
bad = 0
for d in sorted(glob.glob("build/test-image/deflate/*.deflate")):
    raw = open(d[:-8] + ".raw", "rb").read()
    try:
        ok = zlib.decompress(open(d, "rb").read(), -15) == raw
    except zlib.error as e:
        ok = False
    print(("PASS  " if ok else "FAIL  ") + "zlib reads " + d.split("/")[-1])
    bad += not ok
sys.exit(1 if bad else 0)
PY
[ $status -eq 0 ] && echo "image: decoders and compressor all passed" || echo "image: FAILED"
exit $status
