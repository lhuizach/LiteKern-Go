#!/usr/bin/env python3
"""Android sparse images, the format fastboot sends: pack a raw image, or unpack one.

  sparse.py pack RAW SPARSE      zero blocks become FILL chunks, the rest RAW chunks
  sparse.py unpack SPARSE RAW

No "don't care" chunks on purpose: every block of the partition is written, so no old data
(the old system's) is left anywhere. Nura's own image uses just these two chunk types,
which is how we know the phone's lk2nd takes them. Nothing here touches a phone."""
import struct
import sys

BLK = 4096
RAW, FILL, DONT_CARE, CRC = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4
MAGIC = 0xED26FF3A
ZERO = bytes(BLK)
RAW_MAX = 8 * 1024 * 1024               # one raw chunk is at most this
MIN_RUN = 16                            # a run of zero blocks shorter than this stays in the raw chunk


def pack(src, dst):
    size = src.seek(0, 2)
    src.seek(0)
    blocks = (size + BLK - 1) // BLK
    chunks = []                         # (type, blocks, payload-or-None); payload is a list of bytes objects
    raw, raw_blocks, zeros = [], 0, 0

    def flush_raw():
        nonlocal raw, raw_blocks
        if raw_blocks:
            chunks.append((RAW, raw_blocks, raw))
        raw, raw_blocks = [], 0

    def flush_zeros():
        nonlocal zeros
        if zeros >= MIN_RUN:
            flush_raw()
            chunks.append((FILL, zeros, None))
        elif zeros:
            raw.extend([ZERO] * zeros)
            nonlocal raw_blocks
            raw_blocks += zeros
        zeros = 0

    for _ in range(blocks):
        b = src.read(BLK)
        if len(b) < BLK:
            b = b + bytes(BLK - len(b))
        if b == ZERO:
            zeros += 1
            continue
        flush_zeros()
        raw.append(b)
        raw_blocks += 1
        if raw_blocks * BLK >= RAW_MAX:
            flush_raw()
    flush_zeros()
    flush_raw()
    # merge nothing further: fastboot re-splits at its own limit
    dst.write(struct.pack("<IHHHHIIII", MAGIC, 1, 0, 28, 12, BLK, blocks, len(chunks), 0))
    for ctype, n, payload in chunks:
        if ctype == RAW:
            dst.write(struct.pack("<HHII", RAW, 0, n, 12 + n * BLK))
            for b in payload:
                dst.write(b)
        else:
            dst.write(struct.pack("<HHII", FILL, 0, n, 16))
            dst.write(struct.pack("<I", 0))
    return blocks, len(chunks)


def unpack(src, dst):
    magic, major, minor, fhsz, chsz, blk, blocks, nchunks, _crc = struct.unpack("<IHHHHIIII", src.read(28))
    assert magic == MAGIC, "not a sparse image"
    src.seek(fhsz)
    for _ in range(nchunks):
        ctype, _r, n, total = struct.unpack("<HHII", src.read(12))
        if ctype == RAW:
            left = n * blk
            while left:
                b = src.read(min(left, 1 << 22))
                dst.write(b)
                left -= len(b)
        elif ctype == FILL:
            pat = src.read(4) * (blk // 4)
            for _i in range(n):
                dst.write(pat)
        elif ctype == DONT_CARE:
            dst.write(bytes(n * blk))   # shown as zeros here; a real flash would leave what was there
        elif ctype == CRC:
            src.read(4)
        else:
            raise SystemExit("unknown chunk 0x%x" % ctype)
    dst.truncate(blocks * blk)


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] not in ("pack", "unpack"):
        raise SystemExit(__doc__)
    with open(sys.argv[2], "rb") as s, open(sys.argv[3], "wb") as d:
        if sys.argv[1] == "pack":
            b, c = pack(s, d)
            print("sparse: %d blocks (%d MiB), %d chunks" % (b, b * BLK // 2**20, c))
        else:
            unpack(s, d)
            print("unpacked")
