"""Music's icon: a white beamed pair of notes on a purple rounded tile, drawn 4x and scaled down.
Usage: ~/lkx-venv/bin/python tools/make-icon-music.py
Writes assets/icons/source/music-1024.png and assets/icons/music.png (112)."""
from PIL import Image, ImageDraw

K = 4
S = 1024 * K
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
def p(v):
    return v * K
d.rounded_rectangle((p(96), p(104), S - p(96), S - p(88)), radius=p(200), fill=(97, 53, 131, 255))      # its shadow
d.rounded_rectangle((p(96), p(96), S - p(96), S - p(96)), radius=p(200), fill=(145, 65, 172, 255))      # the tile
white = (255, 255, 255, 255)
def head(cx, cy):                                           # a note head, tilted like a real one
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(layer).ellipse((p(cx - 100), p(cy - 72), p(cx + 100), p(cy + 72)), fill=white)
    return layer.rotate(22, center=(p(cx), p(cy)), resample=Image.BICUBIC)
im.alpha_composite(head(372, 712))
im.alpha_composite(head(668, 652))
d = ImageDraw.Draw(im)
d.rectangle((p(436), p(320), p(486), p(712)), fill=white)                                   # the stems
d.rectangle((p(732), p(260), p(782), p(652)), fill=white)
d.polygon([(p(436), p(320)), (p(782), p(250)), (p(782), p(366)), (p(436), p(436))], fill=white)       # the beam
big = im.resize((1024, 1024), Image.LANCZOS)
big.save("assets/icons/source/music-1024.png")
big.resize((112, 112), Image.LANCZOS).save("assets/icons/music.png")
