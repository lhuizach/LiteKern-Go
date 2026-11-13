#!/usr/bin/env bash
# LiteKern Go: loads a page on the phone with lkx-webd (in the Alpine sandbox) and brings back a screenshot.
#   bash phone/sandbox/shot.sh URL OUT.png [WAIT_SECONDS] [WxH]
# URL is as the sandbox sees it: file:///root/x.html (copy files to /extras/alpine/root/ first) or https://...
set -euo pipefail
url=${1:?usage: shot.sh URL OUT.png [WAIT] [WxH]}
out=${2:?usage: shot.sh URL OUT.png [WAIT] [WxH]}
wait_s=${3:-10}
size=${4:-720x1224}
KEY=${LKX_PHONE_KEY:-$HOME/.ssh/litekern-phone}
SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 "${LKX_PHONE:-root@172.16.42.1}")
tmp=$(mktemp)
"${SSH[@]}" "/extras/enter.sh sh -c 'mkdir -p /run/u; chmod 700 /run/u; cd /root; XDG_RUNTIME_DIR=/run/u HOME=/root WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 LIBGL_ALWAYS_SOFTWARE=1 /usr/local/bin/lkx-webd --size $size --wait $wait_s --shot /root/shot.ppm \"$url\"' 2>&1 | grep -v -i -E 'fontconfig|accessib' || true"
"${SSH[@]}" 'cat /extras/alpine/root/shot.ppm' > "$tmp"
python3 - "$tmp" "$out" <<'EOF'
import sys, zlib, struct
d = open(sys.argv[1], "rb").read()
hdr, rest = d.split(b"255\n", 1)
w, h = map(int, hdr.split()[1:3])
raw = b"".join(b"\0" + rest[y*w*3:(y+1)*w*3] for y in range(h))
def ch(t, b):
    c = struct.pack(">I", len(b)) + t + b
    return c + struct.pack(">I", zlib.crc32(t + b))
open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + ch(b"IDAT", zlib.compress(raw, 6)) + ch(b"IEND", b""))
print("wrote", sys.argv[2], w, "x", h)
EOF
rm -f "$tmp"
