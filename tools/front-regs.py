#!/usr/bin/env python3
"""front-regs.py LIB OUT.h: the S5K5E3YX's register tables, read out of the stock firmware's sensor library
(libmmcamera_s5k5e3yx.so, from the firmware's system image: see docs/FRONT-CAMERA.md) as a C header for
phone/kernel's s5k5e3yx driver. Records are 12 bytes (u16 address, u16 data, 8 bytes of zeros) in .data."""
import struct
import sys

d = open(sys.argv[1], "rb").read()[0x3000:0x3000 + 0x12FC]


def run(start, count):
    out = []
    for i in range(count):
        a, v, dl, z = struct.unpack_from("<HHII", d, start - 0x4000 + 12 * i)
        assert z == 0
        out.append((a, v))
    return out


init = run(0x4998, 99)
assert init[-1] == (0x0104, 1)
init = init[:-1]                      # the group hold on: the driver does that itself
full = run(0x4430, 36)
crop = run(0x4044, 35)


def emit(name, regs):
    s = "static const struct s5k5e3yx_reg %s[] = {\n" % name
    for k in range(0, len(regs), 4):
        s += "\t" + " ".join("{ 0x%04x, 0x%02x }," % (a, v & 0xFF) for a, v in regs[k:k + 4]) + "\n"
    return s + "\t{ S5K5E3YX_END, 0 }\n};\n\n"


h = """/* The Samsung S5K5E3YX's register values for the Galaxy A5 (2015)'s front camera, read out of the stock
 * firmware's sensor library (libmmcamera_s5k5e3yx.so; tools/front-regs.py, docs/FRONT-CAMERA.md). The PLL
 * is for a 26 MHz clock (0x0305 = 6, 0x0307 = 0xcf); s5k5e3yx.c changes it for the clock the phone gives. */
#define S5K5E3YX_END 0xffff
struct s5k5e3yx_reg {
	u16 addr;
	u8 val;
};

"""
h += emit("init_regs", init) + emit("mode_2576x1932", full) + emit("mode_1920x1080", crop)
open(sys.argv[2], "w", newline="\n").write(h.rstrip() + "\n")
print(len(init), len(full), len(crop), "registers")
