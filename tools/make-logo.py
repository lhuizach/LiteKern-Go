"""The LiteKern Go logo: a blue G whose bar is a grey pill (LiteKern X's
colours, on its navy tile). Drawn 4x and scaled down, so the edges are smooth.

Usage: python3 tools/make-logo.py   (Pillow: ~/lkx-venv/bin/python)
Writes assets/logo-1024.png, assets/logo.png (256 px: the boot screen,
assets/splash.json) and assets/logo.svg (the same shapes, for the README)."""
import math
from PIL import Image, ImageDraw
K = 4
S = 1024 * K
NAVY, EDGE = (30, 52, 92, 255), (12, 20, 36, 255)
BLUE, GREY = (78, 154, 241, 255), (196, 204, 216, 255)
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.rounded_rectangle((40 * K, 40 * K, S - 40 * K, S - 40 * K), radius=210 * K, fill=EDGE)
d.rounded_rectangle((64 * K, 64 * K, S - 64 * K, S - 64 * K), radius=190 * K, fill=NAVY)
cx = cy = 512 * K
r, w = 270 * K, 130 * K
mid = r - w / 2                             # the curve's centre line
d.arc((cx - r, cy - r, cx + r, cy + r), start=0, end=315, fill=BLUE, width=w)
t = math.radians(315)                       # its top end: round
x, y = cx + mid * math.cos(t), cy + mid * math.sin(t)
d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=BLUE)
# The bar: as thick as the curve, its round end where the curve ends, so the
# two finish flush at the right.
x0, x1 = 560 * K, cx + mid
d.rectangle((x0, cy - w / 2, x1, cy + w / 2), fill=GREY)
for x in (x0, x1):
    d.ellipse((x - w / 2, cy - w / 2, x + w / 2, cy + w / 2), fill=GREY)
big = im.resize((1024, 1024), Image.LANCZOS)
big.save("assets/logo-1024.png")
big.resize((256, 256), Image.LANCZOS).save("assets/logo.png")

# The SVG: the curve from 3 o'clock round to half past one (clockwise, as
# above), its top end round; the bar over its other end.
e = 512 + 205 * math.cos(math.radians(315)), 512 + 205 * math.sin(math.radians(315))
open("assets/logo.svg", "w").write(f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
  <rect x="40" y="40" width="944" height="944" rx="210" fill="#0c1424"/>
  <rect x="64" y="64" width="896" height="896" rx="190" fill="#1e345c"/>
  <path d="M 717 512 A 205 205 0 1 1 {e[0]:.1f} {e[1]:.1f}" fill="none" stroke="#4e9af1" stroke-width="130"/>
  <circle cx="{e[0]:.1f}" cy="{e[1]:.1f}" r="65" fill="#4e9af1"/>
  <rect x="560" y="447" width="157" height="130" fill="#c4ccd8"/>
  <circle cx="560" cy="512" r="65" fill="#c4ccd8"/>
  <circle cx="717" cy="512" r="65" fill="#c4ccd8"/>
</svg>
""")
