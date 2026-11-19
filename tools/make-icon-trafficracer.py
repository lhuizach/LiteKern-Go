"""Traffic Racer's icon: two cars seen from above and behind on a desert road with guardrails, a rock and a
cactus at the side, on a rounded tile. Plain Python (no Pillow needed), supersampled and scaled down.
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

    def trap(ya, yb, l0, r0, l1, r1, col):
        for y in range(max(0, int(ya * f)), min(n, int(yb * f))):
            t = (y + 0.5 - ya * f) / max(1e-9, (yb - ya) * f)
            put(y, int((l0 + (l1 - l0) * t) * f), int((r0 + (r1 - r0) * t) * f), col)

    def mix(c, k):                       # k < 0 darker, k > 0 lighter
        t = abs(k) / 100.0
        target = 255 if k > 0 else 0
        return tuple(int(v + (target - v) * t) for v in c)

    def car(cx, cy, w, h, col, light=True):
        """A car from above and behind: front at the top of the picture."""
        top, bot = cy - h / 2, cy + h / 2
        glass = (28, 42, 64)
        rrect(cx - w / 2 - w * 0.18, top + h * 0.06, cx + w / 2 - w * 0.18, bot + h * 0.06, w * 0.26, (60, 40, 10), 100)
        for wy in (top + h * 0.13, bot - h * 0.30):                 # the wheels show as slivers at the sides
            rrect(cx - w / 2 - w * 0.03, wy, cx - w / 2 + w * 0.09, wy + h * 0.17, 8, (17, 18, 24))
            rrect(cx + w / 2 - w * 0.09, wy, cx + w / 2 + w * 0.03, wy + h * 0.17, 8, (17, 18, 24))
        rrect(cx - w / 2, top, cx + w / 2, bot, w * 0.26, mix(col, -45))
        rrect(cx - w / 2 + 6, top + 6, cx + w / 2 - 6, bot - 6, w * 0.24, col)
        rrect(cx - w * 0.30, top + h * 0.06, cx + w * 0.30, top + h * 0.22, w * 0.12, mix(col, 35))
        rrect(cx - w * 0.36, top + h * 0.27, cx + w * 0.36, top + h * 0.41, w * 0.10, glass)
        rrect(cx - w * 0.34, top + h * 0.40, cx + w * 0.34, top + h * 0.66, w * 0.12, mix(col, 55))
        rrect(cx - w * 0.26, top + h * 0.42, cx - w * 0.08, top + h * 0.64, w * 0.08, mix(col, 100))
        rrect(cx - w * 0.33, top + h * 0.67, cx + w * 0.33, top + h * 0.79, w * 0.10, glass)
        rrect(cx - w / 2 + w * 0.05, bot - h * 0.08, cx - w / 2 + w * 0.29, bot - h * 0.03, 5, (255, 51, 71))
        rrect(cx + w / 2 - w * 0.29, bot - h * 0.08, cx + w / 2 - w * 0.05, bot - h * 0.03, 5, (255, 51, 71))

    # sand, in soft bands
    for y in range(n):
        band = int((y / f) // 90) % 2
        put(y, 0, n, (220, 191, 128) if band == 0 else (211, 181, 116))
    # the road, a little narrower at the top
    cx = 512
    trap(0, 1024, cx - 170 - 70, cx + 170 + 70, cx - 270 - 70, cx + 270 + 70, (144, 153, 168))      # rails
    trap(0, 1024, cx - 170 - 42, cx + 170 + 42, cx - 270 - 42, cx + 270 + 42, (226, 232, 239))
    trap(0, 1024, cx - 170, cx + 170, cx - 270, cx + 270, (77, 80, 89))
    trap(0, 1024, cx - 170 + 14, cx - 170 + 26, cx - 270 + 24, cx - 270 + 40, (241, 241, 234))       # edge lines
    trap(0, 1024, cx + 170 - 26, cx + 170 - 14, cx + 270 - 40, cx + 270 - 24, (241, 241, 234))
    trap(0, 1024, cx - 7, cx + 7, cx - 12, cx + 12, (241, 241, 234))                               # the middle
    for i in range(5):
        ya, yb = 40 + i * 215, 40 + i * 215 + 95
        for k in (-1, 1):
            t0, t1 = ya / 1024.0, yb / 1024.0
            x0 = cx + k * (85 + 50 * t0)
            x1 = cx + k * (85 + 50 * t1)
            trap(ya, yb, x0 - 6 - 4 * t0, x0 + 6 + 4 * t0, x1 - 6 - 4 * t1, x1 + 6 + 4 * t1, (241, 241, 234))
    # a rock and a cactus on the sand
    rrect(104, 690, 192, 760, 28, (134, 96, 59))
    rrect(106, 680, 188, 748, 26, (178, 131, 80))
    rrect(120, 684, 168, 708, 11, (208, 166, 114))
    rrect(840, 330, 872, 490, 16, (63, 138, 74))
    rrect(806, 390, 826, 440, 10, (63, 138, 74))
    rrect(806, 390, 850, 410, 10, (63, 138, 74))
    rrect(886, 370, 906, 420, 10, (63, 138, 74))
    rrect(862, 420, 906, 440, 10, (63, 138, 74))
    # the cars: a blue one ahead, the orange player's below it
    car(405, 380, 150, 215, (59, 130, 246))
    car(632, 730, 195, 280, (255, 162, 31))

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
