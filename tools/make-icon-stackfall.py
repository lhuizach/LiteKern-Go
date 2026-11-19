"""Stackfall's icon: a violet shape falling into a gap in a stack of coloured blocks (with its ghost where it
will land), on a dark navy tile. Drawn with plain Python (no Pillow needed), supersampled and scaled down.
Usage: python3 tools/make-icon-stackfall.py
Writes assets/icons/source/stackfall-1024.png and assets/icons/stackfall.png (112)."""
import math
import struct
import zlib


def render(size, ss):
    n = size * ss
    f = n / 1024.0
    rows = [bytearray(n * 4) for _ in range(n)]          # RGBA, transparent

    def rrect(x0, y0, x1, y1, r, col, alpha=255):
        """A filled rounded rectangle in 1024-space; alpha < 255 blends over what is there."""
        x0, y0, x1, y1, r = x0 * f, y0 * f, x1 * f, y1 * f, r * f
        for y in range(max(0, int(y0)), min(n, int(math.ceil(y1)))):
            yc = y + 0.5
            if yc < y0 or yc > y1:
                continue
            dx = 0.0
            if yc < y0 + r:
                d = y0 + r - yc
                dx = r - math.sqrt(max(0.0, r * r - d * d))
            elif yc > y1 - r:
                d = yc - (y1 - r)
                dx = r - math.sqrt(max(0.0, r * r - d * d))
            xa, xb = max(0, int(math.ceil(x0 + dx - 0.5))), min(n, int(math.floor(x1 - dx + 0.5)))
            if xb <= xa:
                continue
            row = rows[y]
            if alpha == 255:
                row[xa * 4:xb * 4] = bytes((col[0], col[1], col[2], 255)) * (xb - xa)
            else:
                for x in range(xa, xb):
                    i = x * 4
                    for c in range(3):
                        row[i + c] = (row[i + c] * (255 - alpha) + col[c] * alpha) // 255

    def block(x, y, u, col, ghost=False):
        if ghost:
            rrect(x + 6, y + 6, x + u - 6, y + u - 6, 20, col, 70)
            return
        dark = tuple(int(c * 0.55) for c in col)
        light = tuple(min(255, c + (255 - c) * 2 // 5) for c in col)
        rrect(x + 3, y + 3, x + u - 3, y + u - 3, 20, dark)
        rrect(x + 3, y + 3, x + u - 3, y + u - 14, 20, col)
        rrect(x + 20, y + 18, x + 20 + u // 2, y + 18 + 16, 8, light)

    tile = (21, 26, 63)
    rrect(96, 96, 928, 928, 200, tile)
    u, x0 = 112, 176
    gold, green, red, blue, violet = (255, 207, 63), (79, 220, 122), (240, 71, 90), (74, 140, 255), (180, 104, 255)
    # the stack: row A is the bottom row (y 720), row B above it (y 608)
    for col, (cx, cy, c) in enumerate([
        (0, 720, gold), (1, 720, gold), (0, 608, gold), (1, 608, gold),
        (2, 720, green), (4, 720, red), (5, 720, red), (5, 608, blue),
    ]):
        block(x0 + cx * u, cy, u, c)
    # where it will land (the ghost), and the shape itself, still falling
    for cx, cy in [(2, 608), (3, 608), (4, 608), (3, 720)]:
        block(x0 + cx * u, cy, u, violet, ghost=True)
    for cx, cy in [(2, 272), (3, 272), (4, 272), (3, 384)]:
        block(x0 + cx * u, cy, u, violet)

    # scale down: average each ss x ss group (colour weighted by alpha, so the tile's edge stays clean)
    out = bytearray()
    for oy in range(size):
        line = bytearray(size * 4)
        for ox in range(size):
            r = g = b = a = 0
            for sy in range(ss):
                row = rows[oy * ss + sy]
                for sx in range(ss):
                    i = (ox * ss + sx) * 4
                    al = row[i + 3]
                    r += row[i] * al
                    g += row[i + 1] * al
                    b += row[i + 2] * al
                    a += al
            o = ox * 4
            if a:
                line[o], line[o + 1], line[o + 2] = r // a, g // a, b // a
            line[o + 3] = a // (ss * ss)
        out += b"\x00" + line
    return bytes(out)


def png(path, size, raw):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        fh.write(chunk(b"IEND", b""))


png("assets/icons/source/stackfall-1024.png", 1024, render(1024, 2))
png("assets/icons/stackfall.png", 112, render(112, 4))
print("wrote assets/icons/source/stackfall-1024.png and assets/icons/stackfall.png")
