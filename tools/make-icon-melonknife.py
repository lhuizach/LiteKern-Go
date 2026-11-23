"""Melon Knife's icon: a watermelon slice (green rind, white ring, red flesh, seeds) with a knife stuck in it, on a
dark tile, drawn 4x and scaled down. Usage: python tools/make-icon-melonknife.py
Writes assets/icons/source/melonknife-1024.png and assets/icons/melonknife.png (112)."""
import math
from PIL import Image, ImageDraw

K = 4
S = 1024 * K
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
def p(v):
    return int(v * K)
d.rounded_rectangle((p(96), p(96), S - p(96), S - p(96)), radius=p(200), fill=(22, 40, 46, 255))     # the tile
cx, cy = 512, 540
for r, col in ((330, (46, 157, 70)), (300, (241, 246, 232)), (278, (240, 69, 90)), (130, (210, 47, 73)), (112, (240, 69, 90))):
    d.ellipse((p(cx - r), p(cy - r), p(cx + r), p(cy + r)), fill=col + (255,))
for i in range(9):                                                                                  # seeds
    a = math.radians(i * 40 + 10)
    rr = 175 + (i % 2) * 60
    x, y = cx + math.cos(a) * rr, cy + math.sin(a) * rr
    d.ellipse((p(x - 17), p(y - 25), p(x + 17), p(y + 25)), fill=(42, 20, 24, 255))
# the knife, stuck in from the top right
bx, by, tx, ty = 640, 230, 520, 470
d.line((p(tx), p(ty), p(bx), p(by)), fill=(154, 167, 184, 255), width=p(58))
d.line((p(tx), p(ty), p(bx), p(by)), fill=(223, 231, 240, 255), width=p(34))
hx, hy = bx + 90, by - 150
d.line((p(bx), p(by), p(hx), p(hy)), fill=(122, 74, 42, 255), width=p(76))
d.line((p(bx), p(by), p(hx), p(hy)), fill=(165, 105, 60, 255), width=p(30))
big = im.resize((1024, 1024), Image.LANCZOS)
big.save("assets/icons/source/melonknife-1024.png")
big.resize((112, 112), Image.LANCZOS).save("assets/icons/melonknife.png")
