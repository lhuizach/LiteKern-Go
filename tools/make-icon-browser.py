"""The Web app's icon (the browser, linux/overlay): a blue globe, the shapes of
assets/icons/source/browser.svg drawn with Pillow, 4x and scaled down.
Usage: ~/lkx-venv/bin/python tools/make-icon-browser.py
Writes assets/icons/source/browser-1024.png and assets/icons/browser.png (112)."""
from PIL import Image, ImageDraw

K = 1024 * 4 / 48                           # the SVG's units (48) to pixels, 4x
im = Image.new("RGBA", (4096, 4096), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
def p(v):
    return v * K
def circle(cx, cy, r, **kw):
    d.ellipse((p(cx - r), p(cy - r), p(cx + r), p(cy + r)), **kw)
circle(24, 25, 20, fill=(26, 95, 180, 255))         # its shadow
circle(24, 24, 20, fill=(53, 132, 228, 255))
line = (255, 255, 255, 230)
w = int(p(2))
d.ellipse((p(24 - 8.5), p(4), p(24 + 8.5), p(44)), outline=line, width=w)
d.line((p(24), p(4), p(24), p(44)), fill=line, width=w)
for y, x0, x1 in ((15.5, 6.5, 41.5), (24, 4, 44), (32.5, 6.5, 41.5)):
    d.line((p(x0), p(y), p(x1), p(y)), fill=line, width=w)
circle(24, 24, 20, outline=(255, 255, 255, 255), width=w)
big = im.resize((1024, 1024), Image.LANCZOS)
big.save("assets/icons/source/browser-1024.png")
big.resize((112, 112), Image.LANCZOS).save("assets/icons/browser.png")
