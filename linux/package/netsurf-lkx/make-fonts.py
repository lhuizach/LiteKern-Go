#!/usr/bin/env python3
"""LiteKern X - the Web app's fonts (LiteKernX-Roadmap/06, phase L4/L6 polish).

NetSurf draws pages with FreeType, one file per face, and doesn't choose a
variable font's weight. So the desktop's font, Ubuntu Sans (a variable font:
width and weight axes), is cut into the plain faces it needs, at normal
width: Regular, Bold, Italic, Bold Italic, and Ubuntu Sans Mono Regular and
Bold. The results are committed (linux/overlay/usr/share/fonts/ubuntu/, with
the licence); rerun this only for a new version of the font.

Usage (in WSL, needs fontTools):
    python3 linux/package/netsurf-lkx/make-fonts.py /usr/share/fonts/truetype/ubuntu \\
        linux/overlay/usr/share/fonts/ubuntu
"""
import os
import sys

from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

FACES = [   # source, axes, output
    ("UbuntuSans[wdth,wght].ttf", {"wdth": 100, "wght": 400}, "UbuntuSans-Regular.ttf"),
    ("UbuntuSans[wdth,wght].ttf", {"wdth": 100, "wght": 700}, "UbuntuSans-Bold.ttf"),
    ("UbuntuSans-Italic[wdth,wght].ttf", {"wdth": 100, "wght": 400}, "UbuntuSans-Italic.ttf"),
    ("UbuntuSans-Italic[wdth,wght].ttf", {"wdth": 100, "wght": 700}, "UbuntuSans-BoldItalic.ttf"),
    ("UbuntuSansMono[wght].ttf", {"wght": 400}, "UbuntuSansMono-Regular.ttf"),
    ("UbuntuSansMono[wght].ttf", {"wght": 700}, "UbuntuSansMono-Bold.ttf"),
]


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for name, axes, result in FACES:
        font = TTFont(os.path.join(src, name))
        static = instancer.instantiateVariableFont(font, axes, updateFontNames=False)
        static.save(os.path.join(out, result))
        print(f"{result}: {os.path.getsize(os.path.join(out, result)) // 1024} KB")


if __name__ == "__main__":
    main()
