#!/usr/bin/env python3
"""A small device tree dumper (no dtc needed): fdt_dump.py FILE [OFFSET] prints the tree as text, with
properties as strings or as 32-bit numbers. Used to read the stock firmware's camera nodes on the PC."""
import struct
import sys


def dump(data, base=0):
    magic, total, off_struct, off_strings, _, _, _, _, size_strings, size_struct = struct.unpack_from(">10I", data, base)
    assert magic == 0xD00DFEED, "not a device tree"
    strings = data[base + off_strings: base + off_strings + size_strings]
    p = base + off_struct
    depth = 0
    out = []

    def cstr(b, o):
        e = b.index(b"\0", o)
        return b[o:e].decode("latin1")

    while True:
        tag = struct.unpack_from(">I", data, p)[0]
        p += 4
        if tag == 1:
            name = cstr(data, p)
            p = (p + len(name) + 1 + 3) & ~3
            out.append("  " * depth + (name or "/") + " {")
            depth += 1
        elif tag == 2:
            depth -= 1
            out.append("  " * depth + "};")
        elif tag == 3:
            ln, nameoff = struct.unpack_from(">II", data, p)
            p += 8
            val = data[p: p + ln]
            p = (p + ln + 3) & ~3
            name = cstr(strings, nameoff)
            if ln and all(32 <= c < 127 or c == 0 for c in val) and val[-1] == 0 and ln > 1:
                text = '"' + '", "'.join(s.decode() for s in val[:-1].split(b"\0")) + '"'
            elif ln % 4 == 0:
                text = "<" + " ".join("0x%x" % v for v in struct.unpack(">%dI" % (ln // 4), val)) + ">"
            elif ln:
                text = "[" + val.hex() + "]"
            else:
                text = ""
            out.append("  " * depth + (name + " = " + text + ";" if text else name + ";"))
        elif tag == 4:
            continue
        elif tag == 9:
            break
    return "\n".join(out)


if __name__ == "__main__":
    d = open(sys.argv[1], "rb").read()
    print(dump(d, int(sys.argv[2], 0) if len(sys.argv) > 2 else 0))
