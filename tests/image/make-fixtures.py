#!/usr/bin/env python3
"""LiteKern X - make the picture decoder's test files (tests/image/).

Run once, with Pillow (on Windows: python tests/image/make-fixtures.py).
Each picture is cut from a project wallpaper and saved in one of the kinds
apps/files/image.c reads; next to it goes <name>.ref.png, what Pillow
decodes it to (the test compares with that). Odd sizes on purpose: JPEG's
blocks and PNG's bit packing don't divide them evenly.
"""
import io, os, struct, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "assets", "wallpapers", "crossing.png")


def out(name):
    return os.path.join(HERE, "fixtures", name)


def ref(name, im):
    im.convert("RGBA").save(out(name + ".ref.png"))


def exif_orientation(value):
    """A minimal EXIF block (little-endian TIFF) with just the orientation."""
    tiff = b"II*\x00" + struct.pack("<I", 8) + struct.pack("<H", 1)
    tiff += struct.pack("<HHII", 0x0112, 3, 1, value) + struct.pack("<I", 0)
    return b"Exif\x00\x00" + tiff


def main():
    os.makedirs(out(""), exist_ok=True)
    base = Image.open(SRC).convert("RGB")
    pic = base.crop((300, 180, 300 + 157, 180 + 93))         # 157 x 93
    big = base.crop((100, 100, 100 + 333, 100 + 211))

    jpegs = {
        "baseline-420.jpg": dict(quality=90, subsampling=2),
        "baseline-422.jpg": dict(quality=85, subsampling=1),
        "baseline-444.jpg": dict(quality=95, subsampling=0),
        "progressive-420.jpg": dict(quality=88, subsampling=2, progressive=True),
        "progressive-444.jpg": dict(quality=80, subsampling=0, progressive=True),
        "restarts.jpg": dict(quality=90, subsampling=2, restart_marker_blocks=3),
        "restarts-progressive.jpg": dict(quality=90, subsampling=2, progressive=True,
                                         restart_marker_rows=1),
    }
    for name, opts in jpegs.items():
        src = big if "restarts" in name else pic
        src.save(out(name), "JPEG", **opts)
        ref(name, Image.open(out(name)))
    pic.convert("L").save(out("grey.jpg"), "JPEG", quality=90)
    ref("grey.jpg", Image.open(out("grey.jpg")))
    pic.convert("L").save(out("grey-progressive.jpg"), "JPEG", quality=90, progressive=True)
    ref("grey-progressive.jpg", Image.open(out("grey-progressive.jpg")))
    # A camera's "turned on its side": the pixels are stored sideways, and
    # EXIF says to turn them 90 degrees clockwise (orientation 6).
    pic.transpose(Image.Transpose.ROTATE_90).save(out("exif-rotated.jpg"), "JPEG", quality=92,
                                                  exif=exif_orientation(6))
    from PIL import ImageOps
    ref("exif-rotated.jpg", ImageOps.exif_transpose(Image.open(out("exif-rotated.jpg"))))

    rgba = pic.convert("RGBA")
    a = Image.linear_gradient("L").resize(rgba.size)
    rgba.putalpha(a)
    pngs = {
        "rgb.png": (pic, {}),
        "rgba.png": (rgba, {}),
        "rgba-interlaced.png": (rgba, dict(interlace=1)),   # Pillow ignores it; see below
        "palette.png": (pic.convert("P", palette=Image.Palette.ADAPTIVE, colors=200), {}),
        "palette-4bit.png": (pic.convert("P", palette=Image.Palette.ADAPTIVE, colors=16), dict(bits=4)),
        "grey-1bit.png": (pic.convert("1"), {}),
        "grey-alpha.png": (rgba.convert("LA"), {}),
        "rgb16.png": (pic.convert("RGB"), {}),
    }
    for name, (im, opts) in pngs.items():
        im.save(out(name), "PNG", **opts)
    # 16 bits a channel: Pillow can't write RGB16, so build it by hand.
    write_png16(out("rgb16.png"), pic)
    # Interlaced (Adam7): also by hand, since Pillow doesn't write it.
    write_png_interlaced(out("rgba-interlaced.png"), rgba)
    # Palette with transparency (tRNS).
    p = pic.convert("P", palette=Image.Palette.ADAPTIVE, colors=64)
    p.info["transparency"] = 3
    p.save(out("palette-trns.png"), "PNG", transparency=3)
    for name in list(pngs) + ["palette-trns.png"]:
        ref(name, Image.open(out(name)))

    pic.save(out("rgb24.bmp"), "BMP")
    ref("rgb24.bmp", Image.open(out("rgb24.bmp")))
    pic.convert("P", palette=Image.Palette.ADAPTIVE, colors=256).save(out("palette8.bmp"), "BMP")
    ref("palette8.bmp", Image.open(out("palette8.bmp")))
    write_bmp32_topdown(out("xrgb32-topdown.bmp"), pic)
    ref("xrgb32-topdown.bmp", pic)

    with open(out("cut-short.jpg"), "wb") as f:            # shows what's there, like a browser
        f.write(open(out("baseline-420.jpg"), "rb").read()[:900])
    good = bytearray(open(out("baseline-420.jpg"), "rb").read())
    sof = good.find(b"\xff\xc0")
    good[sof + 5:sof + 9] = bytes(4)                        # no width or height
    open(out("damaged.jpg"), "wb").write(good)
    png = bytearray(open(out("rgb.png"), "rb").read())
    png[60:80] = b"\xff" * 20                                # its compressed data scrambled
    open(out("damaged.png"), "wb").write(png)
    with open(out("not-a-picture.png"), "wb") as f:
        f.write(b"Hello, this is just text pretending to be a PNG.\n")
    print("fixtures written to", out(""))


def png_chunk(kind, data):
    import zlib
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)


def write_png16(path, im):
    import zlib
    w, h = im.size
    raw = bytearray()
    px = im.load()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            for c in px[x, y]:
                v = c * 257 ^ (x & 0x5a)                    # low bytes that don't matter
                raw += struct.pack(">H", v)
    data = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 2, 0, 0, 0))
    data += png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + png_chunk(b"IEND", b"")
    open(path, "wb").write(data)


def write_png_interlaced(path, im):
    import zlib
    w, h = im.size
    px = im.load()
    raw = bytearray()
    passes = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
    for x0, y0, dx, dy in passes:
        xs = range(x0, w, dx)
        if not len(xs):
            continue
        prev = None
        for y in range(y0, h, dy):
            row = bytearray()
            for x in xs:
                row += bytes(px[x, y])
            # Sub filter on even rows, Up on odd: exercises two filters.
            if prev is not None and y % 2:
                raw.append(2)
                raw += bytes((row[i] - prev[i]) & 255 for i in range(len(row)))
            else:
                raw.append(1)
                raw += bytes((row[i] - (row[i - 4] if i >= 4 else 0)) & 255 for i in range(len(row)))
            prev = row
    data = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 1))
    data += png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + png_chunk(b"IEND", b"")
    open(path, "wb").write(data)


def write_bmp32_topdown(path, im):
    w, h = im.size
    px = im.load()
    pix = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            pix += bytes((b, g, r, 0))
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(pix), 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0, len(pix), 2835, 2835, 0, 0)
    open(path, "wb").write(hdr + dib + pix)


if __name__ == "__main__":
    sys.exit(main())
