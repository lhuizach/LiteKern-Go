# The GUI font

`ui-font.png` (the glyphs) and `ui-font.json` (their metrics) are **Ubuntu Sans** (version 1.006) rasterised by `tools/font-rasterize.py`, at the sizes and weights the GUI uses (`kernel/text.h`). The build turns them into C (`tools/font2c.py`). The console keeps the video BIOS's 8×16 bitmap font.

- **Font:** Ubuntu Sans, Copyright 2011, 2022, 2023 Canonical Ltd. Designed by Dalton Maag Ltd.
- **Licence:** the Ubuntu Font Licence 1.0, <https://ubuntu.com/legal/font-licence>. It allows the font to be used, bundled and redistributed with software, provided its copyright notice and licence go with it.
- "Ubuntu" and "Canonical" are registered trademarks of Canonical Ltd. LiteKern X is not affiliated with either.

**Licence text:** `LICENCE-ubuntu-font.txt` in this folder (from the Ubuntu Sans repository). Before publishing (Phase 4), also mention the font in the release notes.

`clock-font.png`/`.json` are the lock screen's big clock: the same font, 112 px bold, only digits, `:` and the letters of AM/PM (`--clock`). They're packed into the ramdisk (`fonts/clock.lkxf`) and read when the screen first locks.

To regenerate, for example after changing a size in the tool's `FACES` (needs Pillow):
```
python tools/font-rasterize.py "path/to/UbuntuSans[wdth,wght].ttf" assets/fonts
python tools/font-rasterize.py --clock "path/to/UbuntuSans[wdth,wght].ttf" assets/fonts
```
In WSL the font is at `/usr/share/fonts/truetype/ubuntu/`.

The `clock-hd-*` faces are the lock screen clock's, at 320 px so a stretched clock stays sharp, from Ubuntu Sans and Ubuntu Sans Mono (`--clock=NAME:WEIGHT[:WIDTH[:SIZE]]`). `clock-hd-serif` is **DejaVu Serif Bold**, under the DejaVu fonts licence (a Bitstream Vera derivative, free to use and redistribute): see `LICENCE-dejavu.txt`.
