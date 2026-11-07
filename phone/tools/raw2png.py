#!/usr/bin/env python3
"""LiteKern Go, phone: view a frame from camgrab.

    python3 phone/tools/raw2png.py IN.raw OUT.png [WIDTH HEIGHT STRIDE ORDER]

Packed RAW10 to a half-size PNG: each 2x2 Bayer cell becomes a pixel, with a plain
grey-world white balance and a gamma. ORDER is the cell's layout, BGGR by default.
Prints how bright and how varied the frame is. Needs numpy and Pillow.
"""
import sys

import numpy as np
from PIL import Image

path, out = sys.argv[1], sys.argv[2]
w = int(sys.argv[3]) if len(sys.argv) > 3 else 1280
h = int(sys.argv[4]) if len(sys.argv) > 4 else 720
stride = int(sys.argv[5]) if len(sys.argv) > 5 else 1600
order = sys.argv[6] if len(sys.argv) > 6 else "BGGR"

data = np.fromfile(path, dtype=np.uint8)
rows = data[: stride * h].reshape(h, stride)
# Five bytes hold four pixels: the top 8 bits of each, then their low 2 bits.
n = w * 5 // 4
g = rows[:, :n]
px = np.delete(g, np.arange(4, n, 5), axis=1).astype(np.float32)[:, :w] / 255.0
print("raw mean %.3f, min %.3f, max %.3f, std %.3f" % (px.mean(), px.min(), px.max(), px.std()))

cells = {order[0]: px[0::2, 0::2], order[1]: px[0::2, 1::2], order[3]: px[1::2, 1::2]}
green = (px[0::2, 1::2] + px[1::2, 0::2]) / 2
r, b = cells["R"], cells["B"]
ch, cw = r.shape
r, g, b = r[:ch, :cw], green[:ch, :cw], b[:ch, :cw]
print("channel means r %.3f g %.3f b %.3f" % (r.mean(), g.mean(), b.mean()))
mean = (r.mean() + g.mean() + b.mean()) / 3
rgb = np.stack([r * mean / max(r.mean(), 1e-3), g * mean / max(g.mean(), 1e-3),
                b * mean / max(b.mean(), 1e-3)], axis=-1)
peak = max(np.percentile(rgb, 99.5), 1e-3)
rgb = np.clip(rgb / peak, 0, 1) ** (1 / 2.2)
Image.fromarray((rgb * 255).astype(np.uint8), "RGB").save(out)
print("saved", out)
