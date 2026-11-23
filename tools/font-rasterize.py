#!/usr/bin/env python3
"""LiteKern X - rasterise the GUI font into an atlas (Phase 3 section 5).

Usage: python tools/font-rasterize.py FONT.ttf assets/fonts
       python tools/font-rasterize.py --clock FONT.ttf assets/fonts
       python tools/font-rasterize.py --clock=NAME:WEIGHT[:WIDTH[:SIZE]] FONT.ttf assets/fonts
                (one of the lock screen clock's faces, clock-NAME.*: Settings offers them.
                 WIDTH 0: the font's own; SIZE: px, 180 unless given; 255 px is the most a
                 glyph can be, so about 330 at most)

Needs Pillow (FreeType), so it's run by hand, not by the build: the output
(ui-font.png, a grayscale atlas, and ui-font.json, its metrics) is
committed, and tools/font2c.py turns it into C at build time with only the
standard library. Rerun it only to change the font or the sizes below.

The faces are the text styles the GUI uses (kernel/text.h). Glyphs are
printable ASCII, hinted by FreeType for crispness at 1:1 on the EeePC's panel,
with fractional advances kept (1/64 px) so spacing stays even.

--clock makes the lock screen's big clock instead (clock-font.png/.json):
one large bold face with only what a clock shows (digits, ':', AM/PM),
the other characters left empty. It goes into the kernel only, not into
every app as the UI font does.
"""
import json
import os
import sys

from PIL import Image, ImageFont

FACES = [                       # name, pixel size, weight (variable font's wght axis)
    ("body", 30, 400),          # LiteKern Go: 2x LiteKern X's sizes (UI-PLAN.md)
    ("bold", 30, 700),
    ("small", 26, 400),
    ("heading", 40, 700),
    ("large", 64, 300),
]
CHARS = [chr(c) for c in range(32, 127)]
# Beyond ASCII, after '~': these Unicode characters, reached in C as the bytes
# 0x80, 0x81, ... (kernel/text.h's TEXT_MINUS etc.).
EXTRA = ["−", "×", "÷", "…"]     # minus, times, divide, ellipsis
CHARS += EXTRA
# Then Latin-1, for accents: 0xA0-0xFF, reached in C as those same bytes.
CHARS += [chr(c) for c in range(0xA0, 0x100)]
CLOCK_FACE = ("clock", 180, 700)        # bold (the user asked, 2026-10-09); 180 px on the phone
CLOCK_CHARS = set("0123456789: AMP")



def face(path, size, weight, width=None):
    f = ImageFont.truetype(path, size)
    try:
        axes = f.get_variation_axes()
        values = []
        for a in axes:
            name = a.get("name", b"")
            name = name.decode() if isinstance(name, bytes) else name
            low = name.lower()
            values.append(weight if low.startswith("weight") else
                          width if width and low.startswith("width") else a.get("default", 100))
        f.set_variation_by_axes(values)
    except Exception:
        pass                    # not a variable font: use it as it is
    return f


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--clock")]
    named = [a[8:].split(":") for a in sys.argv[1:] if a.startswith("--clock=")]
    clock = bool(named) or "--clock" in sys.argv
    clock_face = CLOCK_FACE
    width = None
    if named:
        size = int(named[0][3]) if len(named[0]) > 3 else CLOCK_FACE[1]
        clock_face = (named[0][0], size, int(named[0][1]))
        width = int(named[0][2]) if len(named[0]) > 2 and int(named[0][2]) else None
    if len(args) != 2:
        sys.exit("usage: font-rasterize.py [--clock] FONT.ttf OUT_DIR")
    path, out = args
    base = f"clock-{named[0][0]}" if named else "clock-font" if clock else "ui-font"
    os.makedirs(out, exist_ok=True)
    meta = {"version": 1, "source": os.path.basename(path), "extra": EXTRA, "faces": []}
    rows = []                   # (face index, char, mask)
    for name, size, weight in [clock_face] if clock else FACES:
        f = face(path, size, weight, width)
        ascent, descent = f.getmetrics()
        info = {"name": name, "size": size, "weight": weight, "ascent": ascent,
                "descent": descent, "glyphs": []}
        for ch in CHARS[:99] if clock else CHARS:     # the clock's: no Latin-1
            only = set(os.environ["LKX_FONT_CHARS"]) if os.environ.get("LKX_FONT_CHARS") else None   # the setup's hello: its letters
            if clock and ch not in (only or (CLOCK_CHARS - set("AMP") if named else CLOCK_CHARS)):
                info["glyphs"].append({"c": ord(ch), "w": 0, "h": 0, "xoff": 0, "yoff": 0, "adv64": 0})
                rows.append((len(meta["faces"]), ch, None))
                continue
            mask, (xoff, yoff) = f.getmask2(ch, mode="L")
            w, h = mask.size
            img = Image.frombytes("L", (w, h), bytes(mask)) if w and h else None
            if not img:                     # nothing drawn (space): offsets unused, and must fit an int8
                xoff = yoff = 0
            info["glyphs"].append({"c": ord(ch), "w": w, "h": h, "xoff": xoff, "yoff": yoff,
                                   "adv64": round(f.getlength(ch) * 64)})
            rows.append((len(meta["faces"]), ch, img))
        if named:                       # the lock screen's: no empty space above the digits,
            drawn = [g["yoff"] for g in info["glyphs"] if g["w"] and g["h"]]   # so offsets fit a byte
            top = min(drawn) if drawn else 0
            for g in info["glyphs"]:
                if g["w"] and g["h"]:
                    g["yoff"] -= top
            info["ascent"] -= top
        meta["faces"].append(info)

    # Pack glyphs in rows, 512 px wide.
    W, x, y, row_h = 512, 0, 0, 0
    place = []
    for fi, ch, img in rows:
        w, h = img.size if img else (0, 0)
        if x + w > W:
            x, y, row_h = 0, y + row_h + 1, 0
        place.append((x, y))
        x += w + 1
        row_h = max(row_h, h)
    atlas = Image.new("L", (W, y + row_h + 1), 0)
    k = 0
    for fi, info in enumerate(meta["faces"]):
        for g in info["glyphs"]:
            gx, gy = place[k]
            img = rows[k][2]
            if img:
                atlas.paste(img, (gx, gy))
            g["x"], g["y"] = gx, gy
            k += 1
    atlas.save(os.path.join(out, base + ".png"))
    with open(os.path.join(out, base + ".json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(meta, f, indent=1)
    print(f"font-rasterize: {len(meta['faces'])} faces, atlas {W}x{atlas.size[1]} -> {out}/{base}.*")


if __name__ == "__main__":
    main()
