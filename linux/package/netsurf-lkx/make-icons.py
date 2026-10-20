"""LiteKern X: the Web app's toolbar icons, in LiteKern X's style (white line
icons on the dark header grey), drawn at NetSurf's own sizes. NetSurf's
framebuffer front end converts res/icons/*.png at build time; netsurf-lkx.mk
copies these over its own.

Usage: python3 make-icons.py OUTDIR
"""
import math
import os
import sys

from PIL import Image, ImageDraw

S = 8                                   # drawn this much bigger, then scaled down
WHITE = (255, 255, 255, 255)
GREY = (110, 110, 115, 255)             # unavailable (the _g icons)
BG = (46, 46, 50)                       # the header grey: #2e2e32


def canvas(w, h, rgb=False):
    im = Image.new("RGB" if rgb else "RGBA", (w * S, h * S), BG if rgb else (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def done(im, w, h, path):
    im.resize((w, h), Image.LANCZOS).save(path)


def line(d, pts, c, width=2.2):
    d.line([(x * S, y * S) for x, y in pts], fill=c, width=int(width * S), joint="curve")
    for x, y in (pts[0], pts[-1]):          # round ends
        r = width * S / 2
        d.ellipse((x * S - r, y * S - r, x * S + r, y * S + r), fill=c)


def back(c, w=22, h=26):
    im, d = canvas(w, h)
    line(d, [(13.5, 7.5), (8, 13), (13.5, 18.5)], c)
    return im


def forward(c, w=22, h=26):
    im, d = canvas(w, h)
    line(d, [(8.5, 7.5), (14, 13), (8.5, 18.5)], c)
    return im


def history(c, w=26, h=26):
    im, d = canvas(w, h)
    r = 8
    d.ellipse(((13 - r) * S, (13 - r) * S, (13 + r) * S, (13 + r) * S), outline=c, width=int(2.2 * S))
    line(d, [(13, 8.5), (13, 13), (16.5, 15)], c)
    return im


def home(c, w=26, h=26):
    im, d = canvas(w, h)
    line(d, [(5.5, 12.5), (13, 6), (20.5, 12.5)], c)
    line(d, [(8, 11), (8, 20), (18, 20), (18, 11)], c)
    return im


def reload(c, w=24, h=26):
    im, d = canvas(w, h)
    r, cx, cy = 7, 12, 13.5
    pts = [(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))) for a in range(20, 301, 10)]
    line(d, pts, c)
    # the arrow head at the arc's end (300 degrees: up and right), pointing on round
    ex, ey = pts[-1]
    tri = [(ex + 4.2, ey - 1.0), (ex - 1.8, ey - 3.6), (ex - 0.6, ey + 3.4)]
    d.polygon([(x * S, y * S) for x, y in tri], fill=c)
    return im


def stop(c, w=26, h=26):
    im, d = canvas(w, h)
    line(d, [(8, 8), (18, 18)], c)
    line(d, [(18, 8), (8, 18)], c)
    return im


def arrow(direction):
    im, d = canvas(18, 18, rgb=True)
    tri = {"u": [(9, 6), (13, 11.5), (5, 11.5)], "d": [(9, 12), (13, 6.5), (5, 6.5)],
           "l": [(6, 9), (11.5, 5), (11.5, 13)], "r": [(12, 9), (6.5, 5), (6.5, 13)]}[direction]
    d.polygon([(x * S, y * S) for x, y in tri], fill=(200, 200, 204))
    return im


out = sys.argv[1]
os.makedirs(out, exist_ok=True)
for name, fn, w, h in [("back", back, 22, 26), ("forward", forward, 22, 26), ("history", history, 26, 26),
                       ("home", home, 26, 26), ("reload", reload, 24, 26), ("stop", stop, 26, 26)]:
    done(fn(WHITE), w, h, os.path.join(out, name + ".png"))
    done(fn(GREY), w, h, os.path.join(out, name + "_g.png"))
for dname in "udlr":
    done(arrow(dname), 18, 18, os.path.join(out, "scroll" + dname + ".png"))
print("icons written to", out)
