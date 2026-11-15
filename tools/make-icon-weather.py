"""Weather's icon: a sun behind a white cloud on a blue rounded tile, drawn 4x and scaled down.
Usage: ~/lkx-venv/bin/python tools/make-icon-weather.py
Writes assets/icons/source/weather-1024.png and assets/icons/weather.png (112)."""
from PIL import Image, ImageDraw

K = 4
S = 1024 * K
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
def p(v):
    return v * K
d.rounded_rectangle((p(96), p(104), S - p(96), S - p(88)), radius=p(200), fill=(30, 90, 170, 255))       # its shadow
# the tile: a sky, lighter toward the bottom
tile = Image.new("RGBA", (S, S), (0, 0, 0, 0))
td = ImageDraw.Draw(tile)
top, bottom = (58, 140, 235), (122, 190, 250)
for y in range(p(96), S - p(96)):
    t = (y - p(96)) / (S - p(192))
    td.line((0, y, S, y), fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle((p(96), p(96), S - p(96), S - p(96)), radius=p(200), fill=255)
im.paste(tile, (0, 0), mask)
d = ImageDraw.Draw(im)
sun = (255, 205, 60, 255)
cx, cy = p(660), p(380)
d.ellipse((cx - p(150), cy - p(150), cx + p(150), cy + p(150)), fill=sun)
import math
for k in range(8):
    a = k * math.pi / 4
    d.line((cx + math.cos(a) * p(200), cy + math.sin(a) * p(200), cx + math.cos(a) * p(268), cy + math.sin(a) * p(268)),
           fill=sun, width=p(36))
white = (255, 255, 255, 255)
base = p(740)
d.ellipse((p(230), base - p(230), p(470), base + p(10)), fill=white)
d.ellipse((p(340), base - p(360), p(690), base - p(10)), fill=white)
d.ellipse((p(580), base - p(250), p(830), base), fill=white)
d.rectangle((p(350), base - p(190), p(710), base), fill=white)
big = im.resize((1024, 1024), Image.LANCZOS)
big.save("assets/icons/source/weather-1024.png")
big.resize((112, 112), Image.LANCZOS).save("assets/icons/weather.png")
