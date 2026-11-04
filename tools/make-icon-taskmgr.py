"""The Task Manager's icon: a dark tile with three bars and a pulse line
(what's running, and how hard), drawn 4x and scaled down.
Usage: ~/lkx-venv/bin/python tools/make-icon-taskmgr.py
Writes assets/icons/source/taskmgr-1024.png and assets/icons/taskmgr.png (112)."""
from PIL import Image, ImageDraw

K = 4
S = 1024 * K
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
def p(v):
    return v * K
d.rounded_rectangle((p(96), p(96), S - p(96), S - p(96)), radius=p(190), fill=(36, 40, 52, 255))
for x, h, c in ((290, 230, (53, 132, 228)), (470, 380, (46, 194, 126)), (650, 290, (230, 97, 0))):
    d.rounded_rectangle((p(x - 60), p(800 - h), p(x + 60), p(800)), radius=p(30), fill=c + (255,))
pts = [(200, 560), (330, 560), (400, 400), (480, 700), (560, 470), (620, 560), (824, 560)]
d.line([(p(x), p(y)) for x, y in pts], fill=(255, 255, 255, 255), width=p(30), joint="curve")
for x, y in (pts[0], pts[-1]):
    d.ellipse((p(x - 15), p(y - 15), p(x + 15), p(y + 15)), fill=(255, 255, 255, 255))
big = im.resize((1024, 1024), Image.LANCZOS)
big.save("assets/icons/source/taskmgr-1024.png")
big.resize((112, 112), Image.LANCZOS).save("assets/icons/taskmgr.png")
