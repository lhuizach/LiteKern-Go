#!/usr/bin/env python3
"""Android sparse image (simg2img) in Python: unsparse.py IN OUT. Used to read the stock firmware's
system image on the PC (nothing here touches the phone)."""
import struct
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f, open(dst, "wb") as o:
    magic, major, minor, fhs, chs, blk, total_blks, total_chunks, _ = struct.unpack("<IHHHHIIII", f.read(28))
    assert magic == 0xED26FF3A, "not a sparse image"
    f.seek(fhs)
    for _ in range(total_chunks):
        ctype, _, csz, tsz = struct.unpack("<HHII", f.read(12))
        n = csz * blk
        if ctype == 0xCAC1:        # raw
            left = n
            while left:
                b = f.read(min(left, 1 << 22))
                o.write(b)
                left -= len(b)
        elif ctype == 0xCAC2:      # fill
            v = f.read(4)
            o.write(v * (n // 4))
        elif ctype == 0xCAC3:      # don't care
            o.seek(n, 1)
        elif ctype == 0xCAC4:      # crc
            f.read(4)
    o.truncate(total_blks * blk)
