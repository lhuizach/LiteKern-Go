"""Snake's icon: a green snake curled in an S over a dark rounded tile, with an apple, drawn 4x and scaled
down. Usage: ~/lkx-venv/bin/python tools/make-icon-snake.py
Writes assets/icons/source/snake-1024.png and assets/icons/snake.png (112)."""
import math
from PIL import Image, ImageDraw

K = 4
S = 1024 * K
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
def p(v):
    return int(v * K)
d.rounded_rectangle((p(96), p(96), S - p(96), S - p(96)), radius=p(200), fill=(20, 59, 65, 255))     # the tile
# the body: an S through a list of circles, tail to head, alternating shades
pts = []
for i in range(60):
    t = i / 59
    x = 512 + math.sin(t * math.pi * 2.0 + math.pi) * 215
    y = 250 + t * 540
    pts.append((x, y))
for i, (x, y) in enumerate(pts):
    r = 62 - (1 - i / 59) * 26 if i < 59 else 78
    col = (63, 212, 122) if (i // 3) % 2 == 0 else (52, 187, 105)
    if i == 59:
        col = (99, 234, 149)
    d.ellipse((p(x - r), p(y - r), p(x + r), p(y + r)), fill=col + (255,))
hx, hy = pts[-1]
for dx in (-30, 30):
    d.ellipse((p(hx + dx - 22), p(hy - 14 - 22), p(hx + dx + 22), p(hy - 14 + 22)), fill=(255, 255, 255, 255))
    d.ellipse((p(hx + dx - 10), p(hy - 8 - 10), p(hx + dx + 10), p(hy - 8 + 10)), fill=(7, 33, 26, 255))
# the apple, top right
ax, ay, ar = 735, 300, 78
d.ellipse((p(ax - ar), p(ay - ar), p(ax + ar), p(ay + ar)), fill=(240, 71, 90, 255))
d.ellipse((p(ax - 40), p(ay - 44), p(ax - 14), p(ay - 18)), fill=(255, 154, 166, 255))
d.line((p(ax), p(ay - ar + 6), p(ax + 18), p(ay - ar - 40)), fill=(107, 74, 42, 255), width=p(12))
d.ellipse((p(ax + 12), p(ay - ar - 52), p(ax + 70), p(ay - ar - 14)), fill=(95, 224, 138, 255))
big = im.resize((1024, 1024), Image.LANCZOS)
big.save("assets/icons/source/snake-1024.png")
big.resize((112, 112), Image.LANCZOS).save("assets/icons/snake.png")
