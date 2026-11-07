#!/usr/bin/env python3
"""LiteKern Go - draw the phone's wallpapers, 720x1280 portrait.

    ~/lkx-venv/bin/python tools/make-wallpapers-phone.py
    # assets/wallpapers/{veil,lumen,halo,ember}.png

Drawn from maths (no source images, so they're ours to ship): dark, soft and
layered, the top of the picture calm for the clock and the status bar. An
ordered dither stops the dark gradients banding and keeps the PNGs small.

  veil   grey translucent shapes on black, lit along their edges
  lumen  the same, in deep blue and violet
  halo   a wide crescent swept from teal to lilac
  ember  layered shapes in amber and rose on near-black

Needs numpy and Pillow.
"""
import os

import numpy as np
from PIL import Image

W, H = 720, 1280
HERE = os.path.dirname(os.path.abspath(__file__))
Y, X = np.mgrid[0:H, 0:W].astype(np.float64)
BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.0 - 0.47


def rgb(hex_colour):
    return np.array([(hex_colour >> 16) & 255, (hex_colour >> 8) & 255, hex_colour & 255]) / 255.0


def smooth(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def screen(under, over_colour, amount):
    """Light added: `amount` of the colour, screened over what's there."""
    return 1 - (1 - under) * (1 - over_colour * amount[..., None])


def blob_edge(cx, cy, radius, waves):
    """Signed distance (px, negative inside) of a wavy closed shape."""
    dx, dy = X - cx, Y - cy
    dist, ang = np.hypot(dx, dy), np.arctan2(dy, dx)
    r = np.full_like(dist, 1.0)
    for k, amp, phase in waves:
        r += amp * np.sin(k * ang + phase)
    return dist - radius * r


def veil_layer(img, shape, colour, fill, rim, spread):
    """A translucent sheet: a faint fill, brightest just inside its edge."""
    inside = smooth(1.5, -1.5, shape)
    glow = np.exp(np.minimum(shape, 0) / spread)
    return screen(img, colour, inside * (fill + rim * glow))


def background(top, bottom):
    t = smooth(0, H, Y)[..., None]
    return top * (1 - t) + bottom * t


def finish(img, name):
    px = np.clip(img, 0, 1) * 255
    px = px + BAYER[(Y.astype(int) % 4), (X.astype(int) % 4)][..., None]
    out = os.path.join(HERE, "..", "assets", "wallpapers", name + ".png")
    Image.fromarray(np.clip(np.round(px), 0, 255).astype(np.uint8), "RGB").save(out, optimize=True)
    print(name, os.path.getsize(out) // 1024, "KB")


def veils(name, top, bottom, colours, sheets):
    img = background(rgb(top), rgb(bottom))
    for (cx, cy, r, waves, fill, rim, spread), c in zip(sheets, colours):
        img = veil_layer(img, blob_edge(cx, cy, r, waves), rgb(c), fill, rim, spread)
    finish(img, name)


def halo():
    img = background(rgb(0x02060c), rgb(0x03101a))
    # The wide glow behind it, low on the screen.
    img = screen(img, rgb(0x0b4a5c), np.exp(-(((X - 560) / 620) ** 2 + ((Y - 900) / 520) ** 2)) * 0.55)
    cx, cy, r = 300.0, 860.0, 520.0
    disc = smooth(1.5, -1.5, np.hypot(X - cx, Y - cy) - r)
    bite = smooth(-1.5, 1.5, np.hypot(X - cx - 150, Y - cy + 120) - r * 0.78)
    shape = disc * bite
    # Swept round the shape's own centre, teal through blue to lilac.
    sweep = ((np.arctan2(Y - cy, X - cx) + 2.6) / (2 * np.pi)) % 1.0
    stops = [rgb(0x041c2c), rgb(0x0a6a78), rgb(0x28e0c4), rgb(0x3f7fb0), rgb(0xb892d0), rgb(0x3a2a48)]
    pos = sweep * (len(stops) - 1)
    low = np.floor(pos).astype(int)
    frac = (pos - low)[..., None]
    table = np.array(stops)
    colour = table[low] * (1 - frac) + table[np.minimum(low + 1, len(stops) - 1)] * frac
    # Brightest on the inner edge, falling away to the outside.
    inner = np.exp(-np.maximum(r * 0.78 - np.hypot(X - cx - 150, Y - cy + 120), -400) * 0 -
                   np.abs(np.hypot(X - cx - 150, Y - cy + 120) - r * 0.78) / 150)
    lit = (0.32 + 0.68 * inner)[..., None]
    img = img * (1 - shape[..., None]) + colour * lit * shape[..., None]
    # The thin bright line along the crescent's inner edge.
    line = np.exp(-(np.abs(np.hypot(X - cx - 150, Y - cy + 120) - r * 0.78) / 3.0) ** 2) * disc
    img = screen(img, colour * 0.9 + 0.1, line * 0.55)
    finish(img, "halo")


veils("veil", 0x070708, 0x000000, [0x6e6e74, 0x8a8a92, 0x9a9aa2, 0xb4b4bc], [
    (760, 330, 640, [(2, 0.13, 0.6), (3, 0.08, 2.1), (5, 0.03, 0.4)], 0.07, 0.18, 150),
    (-40, 880, 300, [(2, 0.09, 1.2), (3, 0.06, 0.2)], 0.10, 0.34, 70),
    (560, 900, 390, [(2, 0.12, 3.3), (3, 0.07, 1.4), (4, 0.03, 2.6)], 0.09, 0.30, 90),
    (220, 1240, 310, [(2, 0.08, 0.3), (3, 0.05, 2.0)], 0.10, 0.42, 60),
])
veils("lumen", 0x05060f, 0x03020c, [0x24409c, 0x4b3bb0, 0x2c7fd0, 0x8a5ae0], [
    (-60, 420, 620, [(2, 0.12, 1.9), (3, 0.07, 0.3), (5, 0.03, 1.2)], 0.09, 0.22, 140),
    (780, 800, 380, [(2, 0.10, 0.9), (3, 0.07, 2.4)], 0.12, 0.40, 80),
    (260, 1000, 300, [(2, 0.11, 2.2), (3, 0.05, 0.8)], 0.12, 0.40, 70),
    (620, 1300, 340, [(2, 0.09, 1.6), (3, 0.06, 3.0)], 0.12, 0.48, 60),
])
veils("ember", 0x0a0605, 0x050203, [0x7a2d18, 0xb0481c, 0xc2305a, 0xf0a040], [
    (720, 420, 600, [(2, 0.14, 2.7), (3, 0.07, 1.0), (5, 0.03, 0.2)], 0.08, 0.20, 140),
    (40, 860, 350, [(2, 0.10, 0.4), (3, 0.08, 1.9)], 0.10, 0.34, 80),
    (640, 1120, 360, [(2, 0.11, 1.1), (3, 0.06, 2.8)], 0.11, 0.40, 70),
    (190, 1320, 300, [(2, 0.08, 2.0), (3, 0.05, 0.5)], 0.10, 0.50, 55),
])
halo()
