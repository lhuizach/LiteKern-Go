#!/usr/bin/env python3
"""LiteKern Go - builds the shell redesign mockup into one HTML file.

Usage: python3 tools/build-mock.py SHOTS_DIR OUT.html

Embeds the real app icons, wallpapers and screenshots of the current shell
(SHOTS_DIR/*.png) into docs/mockups/shell-redesign.src.html. Needs Pillow.
"""
import base64
import glob
import io
import json
import os
import sys

from PIL import Image


def uri(img, kind, **opts):
    buf = io.BytesIO()
    img.save(buf, kind, **opts)
    mime = "jpeg" if kind == "JPEG" else "png"
    return f"data:image/{mime};base64,{base64.b64encode(buf.getvalue()).decode()}"


def main():
    shots, out = sys.argv[1], sys.argv[2]
    assets = {}
    for path in glob.glob("assets/icons/*.png"):
        name = os.path.basename(path)[:-4]
        img = Image.open(path).convert("RGBA").resize((96, 96), Image.LANCZOS)
        assets["i-" + name] = uri(img, "PNG", optimize=True)
    for path in glob.glob("assets/wallpapers/*.png"):
        name = os.path.basename(path)[:-4]
        img = Image.open(path).convert("RGB")
        # Portrait, cropped as the phone does: the middle of the picture
        side = img.height * 9 // 16
        left = (img.width - side) // 2
        img = img.crop((left, 0, left + side, img.height)).resize((450, 800), Image.LANCZOS)
        assets["w-" + name] = uri(img, "JPEG", quality=78)
    for path in glob.glob(os.path.join(shots, "*.png")):
        name = os.path.basename(path)[:-4]
        img = Image.open(path).convert("RGB").resize((432, 768), Image.LANCZOS)
        assets[name] = uri(img, "JPEG", quality=80)
    src = open("docs/mockups/shell-redesign.src.html", encoding="utf-8").read()
    html = src.replace("/*ASSETS*/{}", json.dumps(assets))
    with open(out, "w", encoding="utf-8") as f:
        f.write(html)
    print(len(assets), "assets,", len(html) // 1024, "KB")


if __name__ == "__main__":
    main()
