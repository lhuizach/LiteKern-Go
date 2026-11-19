"""Traffic Racer's icon: a red car seen from behind on a road that runs off to the horizon, with hills and a
sun, on a rounded tile. Plain Python (no Pillow needed), supersampled and scaled down.
Usage: python3 tools/make-icon-trafficracer.py
Writes assets/icons/source/trafficracer-1024.png and assets/icons/trafficracer.png (112)."""
import math
import struct
import zlib


def render(size, ss):
    n = size * ss
    f = n / 1024.0
    rows = [bytearray(n * 4) for _ in range(n)]

    def put(y, xa, xb, col, alpha=255):
        xa, xb = max(0, xa), min(n, xb)
        if xb <= xa or y < 0 or y >= n:
            return
        row = rows[y]
        if alpha == 255:
            row[xa * 4:xb * 4] = bytes((col[0], col[1], col[2], 255)) * (xb - xa)
        else:
            for x in range(xa, xb):
                i = x * 4
                for c in range(3):
                    row[i + c] = (row[i + c] * (255 - alpha) + col[c] * alpha) // 255
                row[i + 3] = 255

    def rrect(x0, y0, x1, y1, r, col, alpha=255):
        x0, y0, x1, y1, r = x0 * f, y0 * f, x1 * f, y1 * f, r * f
        r = min(r, (x1 - x0) / 2, (y1 - y0) / 2)
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
            put(y, int(math.ceil(x0 + dx - 0.5)), int(math.floor(x1 - dx + 0.5)), col, alpha)

    def circle(cx, cy, r, col):
        rrect(cx - r, cy - r, cx + r, cy + r, r, col)

    def trap(ya, yb, l0, r0, l1, r1, col):
        """Rows ya..yb (1024-space), the left and right edges running from l0/r0 at ya to l1/r1 at yb."""
        for y in range(max(0, int(ya * f)), min(n, int(yb * f))):
            t = (y + 0.5 - ya * f) / max(1e-9, (yb - ya) * f)
            put(y, int((l0 + (l1 - l0) * t) * f), int((r0 + (r1 - r0) * t) * f), col)

    # sky, from blue to haze
    for y in range(int(96 * f), int(500 * f)):
        t = (y - 96 * f) / (404 * f)
        put(y, 0, n, tuple(int(a + (b - a) * t) for a, b in zip((47, 111, 196), (169, 201, 235))))
    circle(700, 380, 52, (255, 241, 184))
    for cx, cy, r, col in [(180, 520, 130, (111, 147, 194)), (410, 540, 110, (111, 147, 194)),
                           (640, 530, 140, (111, 147, 194)), (860, 540, 120, (111, 147, 194)),
                           (270, 560, 120, (79, 125, 155)), (560, 570, 100, (79, 125, 155)),
                           (800, 570, 130, (79, 125, 155))]:
        circle(cx, cy, r, col)
    # grass
    for y in range(int(480 * f), n):
        band = int((y / f - 480) // 70) % 2
        put(y, 0, n, (47, 145, 80) if band == 0 else (42, 134, 72))
    # the road, running to a point at the horizon
    top, bot, hw_top, hw_bot = 480, 940, 12, 470
    trap(top, bot, 512 - hw_top - 6, 512 + hw_top + 6, 512 - hw_bot - 70, 512 + hw_bot + 70, (240, 240, 240))
    # red and white rumble strips, in bands
    for i in range(7):
        ya, yb = top + (bot - top) * (i / 7.0) ** 1.6, top + (bot - top) * ((i + 1) / 7.0) ** 1.6
        if i % 2 == 0:
            def edge(y):
                t = (y - top) / (bot - top)
                return (hw_top + 6 + (hw_bot + 70 - hw_top - 6) * t, hw_top + (hw_bot - hw_top) * t)
            (a0, b0), (a1, b1) = edge(ya), edge(yb)
            trap(ya, yb, 512 - a0, 512 - b0, 512 - a1, 512 - b1, (217, 72, 79))
            trap(ya, yb, 512 + b0, 512 + a0, 512 + b1, 512 + a1, (217, 72, 79))
    trap(top, bot, 512 - hw_top, 512 + hw_top, 512 - hw_bot, 512 + hw_bot, (90, 94, 106))
    # lane dashes
    for k in (-1, 0, 1):
        for i in range(0, 6):
            ya = top + (bot - top) * (i / 6.0) ** 1.7 + 4
            yb = top + (bot - top) * ((i + 0.45) / 6.0) ** 1.7
            wa, wb = 1 + 9 * ((ya - top) / (bot - top)), 1 + 9 * ((yb - top) / (bot - top))
            ca = (ya - top) / (bot - top) * hw_bot * k * 0.5 + hw_top * k * 0.5 * (1 - (ya - top) / (bot - top))
            cb = (yb - top) / (bot - top) * hw_bot * k * 0.5 + hw_top * k * 0.5 * (1 - (yb - top) / (bot - top))
            trap(ya, yb, 512 + ca - wa, 512 + ca + wa, 512 + cb - wb, 512 + cb + wb, (246, 242, 210))

    # the car, from behind
    cx, by, w, h = 512, 905, 330, 270
    rrect(cx - w * 0.6, by - h * 0.07, cx + w * 0.6, by + h * 0.07, h * 0.07, (20, 22, 28), 110)
    rrect(cx - w * 0.53, by - h * 0.24, cx - w * 0.36, by, 14, (12, 13, 18))
    rrect(cx + w * 0.36, by - h * 0.24, cx + w * 0.53, by, 14, (12, 13, 18))
    red = (232, 56, 79)
    rrect(cx - w / 2, by - h * 0.60, cx + w / 2, by - h * 0.12, 30, red)
    rrect(cx - w * 0.39, by - h, cx + w * 0.39, by - h * 0.44, 30, (150, 32, 50))
    rrect(cx - w * 0.34, by - h * 0.92, cx + w * 0.34, by - h * 0.55, 22, (27, 42, 64))
    rrect(cx - w * 0.31, by - h * 0.88, cx + w * 0.31, by - h * 0.82, 6, (74, 98, 134))
    rrect(cx - w / 2, by - h * 0.24, cx + w / 2, by - h * 0.12, 18, (150, 32, 50))
    rrect(cx - w * 0.46, by - h * 0.50, cx - w * 0.22, by - h * 0.36, 8, (255, 51, 71))
    rrect(cx + w * 0.22, by - h * 0.50, cx + w * 0.46, by - h * 0.36, 8, (255, 51, 71))
    rrect(cx - w * 0.1, by - h * 0.37, cx + w * 0.1, by - h * 0.29, 5, (242, 242, 242))

    # scale down; the rounded tile is a mask
    def inside(px, py):
        x, y = px / n * 1024.0, py / n * 1024.0
        if x < 96 or x > 928 or y < 96 or y > 928:
            return False
        cxm = min(max(x, 96 + 200), 928 - 200)
        cym = min(max(y, 96 + 200), 928 - 200)
        return (x - cxm) ** 2 + (y - cym) ** 2 <= 200 ** 2

    out = bytearray()
    for oy in range(size):
        line = bytearray(size * 4)
        for ox in range(size):
            r = g = b = a = 0
            for sy in range(ss):
                row = rows[oy * ss + sy]
                for sx in range(ss):
                    px = ox * ss + sx
                    if not inside(px + 0.5, oy * ss + sy + 0.5):
                        continue
                    i = px * 4
                    r += row[i]
                    g += row[i + 1]
                    b += row[i + 2]
                    a += 255
            o = ox * 4
            if a:
                cnt = a // 255
                line[o], line[o + 1], line[o + 2] = r // cnt, g // cnt, b // cnt
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


png("assets/icons/source/trafficracer-1024.png", 1024, render(1024, 2))
png("assets/icons/trafficracer.png", 112, render(112, 4))
print("wrote assets/icons/source/trafficracer-1024.png and assets/icons/trafficracer.png")
