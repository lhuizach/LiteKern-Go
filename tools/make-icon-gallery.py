"""Gallery's icon: a photo (hills under a sun) on a rounded tile, drawn 4x
and scaled down. Usage: ~/lkx-venv/bin/python tools/make-icon-gallery.py
Writes assets/icons/source/gallery-1024.png and assets/icons/gallery.png (112)."""
from PIL import Image, ImageDraw

K = 4
S = 1024 * K
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
def p(v):
    return v * K
d.rounded_rectangle((p(96), p(96), S - p(96), S - p(96)), radius=p(170), fill=(255, 255, 255, 255))     # the photo
inner = (p(150), p(150), S - p(150), S - p(150))
d.rounded_rectangle(inner, radius=p(120), fill=(98, 160, 234, 255))                                   # sky
d.ellipse((p(610), p(250), p(760), p(400)), fill=(248, 228, 92, 255))                                   # the sun
hills = Image.new("RGBA", (S, S), (0, 0, 0, 0))
h = ImageDraw.Draw(hills)
h.polygon([(p(150), p(874)), (p(150), p(770)), (p(390), p(470)), (p(660), p(800)), (p(660), p(874))],
          fill=(38, 140, 60, 255))                                                        # the far hill
h.polygon([(p(300), p(874)), (p(640), p(530)), (p(874), p(790)), (p(874), p(874))], fill=(52, 176, 90, 255))
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle(inner, radius=p(120), fill=255)   # hills inside the sky's corners
im.paste(hills, (0, 0), Image.composite(hills.split()[3], Image.new("L", (S, S), 0), mask))
big = im.resize((1024, 1024), Image.LANCZOS)
big.save("assets/icons/source/gallery-1024.png")
big.resize((112, 112), Image.LANCZOS).save("assets/icons/gallery.png")
