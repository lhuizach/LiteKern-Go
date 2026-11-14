"""Brickfit's icon: a falling piece over a half-full well of bricks, on a dark rounded tile, drawn 4x
and scaled down. Usage: ~/lkx-venv/bin/python tools/make-icon-brickfit.py
Writes assets/icons/source/brickfit-1024.png and assets/icons/brickfit.png (112)."""
from PIL import Image, ImageDraw

K = 4
S = 1024 * K
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
def p(v):
    return v * K
d.rounded_rectangle((p(96), p(96), S - p(96), S - p(96)), radius=p(200), fill=(36, 39, 58, 255))        # the tile
CY, BL, GR, OR, YE, RE = (87, 227, 220), (53, 132, 228), (51, 209, 122), (255, 163, 72), (246, 211, 45), (237, 51, 59)
cell, gap, x0, y0 = 150, 14, 512 - 2 * 150 - 14 * 3 // 2 + 7, 238
grid = [
    [None, CY, None, None],
    [CY, CY, CY, None],
    [OR, BL, BL, YE],
    [OR, RE, GR, YE],
]
for r, row in enumerate(grid):
    for c, col in enumerate(row):
        if col is None:
            continue
        x, y = x0 + c * (cell + gap), y0 + r * (cell + gap) + (0 if r > 1 else -20)
        d.rounded_rectangle((p(x), p(y), p(x + cell), p(y + cell)), radius=p(34), fill=col + (255,))
        d.rounded_rectangle((p(x + 14), p(y + 14), p(x + cell - 14), p(y + 40)), radius=p(12),
                            fill=tuple(min(255, v + 40) for v in col) + (255,))                    # a light top edge
big = im.resize((1024, 1024), Image.LANCZOS)
big.save("assets/icons/source/brickfit-1024.png")
big.resize((112, 112), Image.LANCZOS).save("assets/icons/brickfit.png")
