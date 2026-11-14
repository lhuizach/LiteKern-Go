"""The Browser app's icon (WebKit, real JavaScript): a bright globe on a dark rounded tile, so it
isn't mistaken for the old Web icon (a bare globe, NetSurf). Drawn 4x and scaled down.
Usage: ~/lkx-venv/bin/python tools/make-icon-browser-app.py
Writes assets/icons/source/webkit-1024.png and assets/icons/webkit.png (112)."""
from PIL import Image, ImageDraw

K = 4
S = 1024 * K
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
def p(v):
    return v * K
d.rounded_rectangle((p(96), p(96), S - p(96), S - p(96)), radius=p(200), fill=(36, 39, 58, 255))        # the tile
cx, cy, r = 512, 512, 290
d.ellipse((p(cx - r), p(cy - r), p(cx + r), p(cy + r)), fill=(53, 132, 228, 255))                       # the globe
line = (255, 255, 255, 235)
w = p(22)
d.ellipse((p(cx - r * 0.45), p(cy - r), p(cx + r * 0.45), p(cy + r)), outline=line, width=int(w))
d.line((p(cx), p(cy - r), p(cx), p(cy + r)), fill=line, width=int(w))
for dy, half in ((-0.5, 0.87), (0, 1.0), (0.5, 0.87)):
    d.line((p(cx - r * half), p(cy + r * dy), p(cx + r * half), p(cy + r * dy)), fill=line, width=int(w))
d.ellipse((p(cx - r), p(cy - r), p(cx + r), p(cy + r)), outline=(255, 255, 255, 255), width=int(w))
d.ellipse((p(660), p(600), p(840), p(780)), fill=(246, 211, 45, 255))                                   # a lit spot: it runs scripts
d.polygon([(p(765), p(622)), (p(718), p(708)), (p(758), p(708)), (p(738), p(762)), (p(800), p(668)), (p(758), p(668)), (p(788), p(622))], fill=(36, 39, 58, 255))
big = im.resize((1024, 1024), Image.LANCZOS)
big.save("assets/icons/source/webkit-1024.png")
big.resize((112, 112), Image.LANCZOS).save("assets/icons/webkit.png")
