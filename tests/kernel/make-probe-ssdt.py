#!/usr/bin/env python3
"""LiteKern X - a test SSDT for the hardware probe (kernel/hwprobe.h).

QEMU's own tables have no embedded controller, battery, thermal zone or
P-states, so the probe's paths for them would go untested. This writes an
SSDT declaring them, as an Eee PC's tables do, for QEMU's -acpitable:

    Scope (\\_SB) {
        Device (EC0) {                              // PNP0C09, ports 0x62/0x66
            Name (_HID, EisaId ("PNP0C09"))
            Name (_CRS, ResourceTemplate () { IO (Decode16, 0x62, 0x62, 0, 1)
                                              IO (Decode16, 0x66, 0x66, 0, 1) })
            OperationRegion (ECOR, EmbeddedControl, 0, 0xFF)
            Field (ECOR, ByteAcc, Lock, Preserve) {
                Offset (0x60), TAH0, 8, FANS, 8, , 3, ACAP, 1,
                Offset (0x93), RPM0, 16 }
        }
        Device (BAT0) { Name (_HID, EisaId ("PNP0C0A")) }
        Device (AC0)  { Name (_HID, "ACPI0003") }
        Device (ATKD) { Name (_HID, "ASUS010") }
    }
    Scope (\\_TZ) {
        ThermalZone (TZ00) { Name (_CRT, 3732)                 // 100 C
                             Method (_PSV) { Return (3632) } }  // 90 C
    }
    Processor (\\_PR.CPU0, 1, 0x810, 6) {
        Name (_PSS, Package () {
            Package () { 1666, 2500, 10, 10, 0x0a26, 0x0a26 },
            Package () { 1000, 1100, 10, 10, 0x0613, 0x0613 } })
    }

Hand-assembled (ACPI 6.x chapter 20), so it needs no iasl.

    python3 tests/kernel/make-probe-ssdt.py OUT.aml
"""
import struct, sys


def pkg(body):
    """PkgLength + body: the length counts the PkgLength bytes too."""
    for n in range(4):
        total = len(body) + 1 + n
        if n == 0 and total < 64:
            return bytes([total]) + body
        if n and total < 1 << (4 + 8 * n):
            lead = (n << 6) | (total & 0x0F)
            rest = [(total >> (4 + 8 * i)) & 0xFF for i in range(n)]
            return bytes([lead] + rest) + body
    raise ValueError("too long")


def seg(name):
    return name.ljust(4, "_").encode()


def path(*names, root=False):
    """\\NAME.NAME...: one segment plain, two DualNamePrefix, more MultiNamePrefix."""
    b = b"\\" if root else b""
    if len(names) == 1:
        return b + seg(names[0])
    if len(names) == 2:
        return b + b"\x2e" + seg(names[0]) + seg(names[1])
    return b + b"\x2f" + bytes([len(names)]) + b"".join(seg(n) for n in names)


def integer(v):
    if v in (0, 1):
        return bytes([v])
    if v < 0x100:
        return b"\x0a" + bytes([v])
    if v < 0x10000:
        return b"\x0b" + struct.pack("<H", v)
    return b"\x0c" + struct.pack("<I", v)


def eisaid(s):
    v = ((ord(s[0]) - 0x40) << 26 | (ord(s[1]) - 0x40) << 21 | (ord(s[2]) - 0x40) << 16 |
         int(s[3:], 16))
    return b"\x0c" + struct.pack(">I", v)          # stored byte-swapped


def string(s):
    return b"\x0d" + s.encode() + b"\x00"


def name(n, data):
    return b"\x08" + seg(n) + data


def scope(p, body):
    return b"\x10" + pkg(p + body)


def device(n, body):
    return b"\x5b\x82" + pkg(seg(n) + body)


def package(*elements):
    return b"\x12" + pkg(bytes([len(elements)]) + b"".join(elements))


def method(n, body, flags=0):
    return b"\x14" + pkg(seg(n) + bytes([flags]) + body)


def io(port):
    return bytes([0x47, 0x01]) + struct.pack("<HHBB", port, port, 0, 1)


def buffer(data):
    return b"\x11" + pkg(integer(len(data)) + data)


def bits_len(bits):
    """A field's width is a PkgLength holding the bit count itself."""
    if bits < 64:
        return bytes([bits])
    return bytes([0x40 | (bits & 0x0F), bits >> 4])


def main():
    crs = buffer(io(0x62) + io(0x66) + b"\x79\x00")
    field_list = (b"\x00" + bits_len(0x60 * 8) +                  # Offset (0x60)
                  seg("TAH0") + bits_len(8) + seg("FANS") + bits_len(8) +
                  b"\x00" + bits_len(3) + seg("ACAP") + bits_len(1) +
                  b"\x00" + bits_len((0x93 - 0x62) * 8 - 4) +     # Offset (0x93)
                  seg("RPM0") + bits_len(16))
    ec = device("EC0",
                name("_HID", eisaid("PNP0C09")) +
                name("_CRS", crs) +
                b"\x5b\x80" + seg("ECOR") + b"\x03" + integer(0) + integer(0xFF) +
                b"\x5b\x81" + pkg(seg("ECOR") + b"\x11" + field_list))
    sb = scope(path("_SB", root=True),
               ec +
               device("BAT0", name("_HID", eisaid("PNP0C0A"))) +
               device("AC0", name("_HID", string("ACPI0003"))) +
               device("ATKD", name("_HID", string("ASUS010"))))
    tz = scope(path("_TZ", root=True),
               b"\x5b\x85" + pkg(seg("TZ00") + name("_CRT", integer(3732)) +
                                 method("_PSV", b"\xa4" + integer(3632))))
    pss = name("_PSS", package(
        package(*(integer(v) for v in (1666, 2500, 10, 10, 0x0A26, 0x0A26))),
        package(*(integer(v) for v in (1000, 1100, 10, 10, 0x0613, 0x0613)))))
    cpu = b"\x5b\x83" + pkg(path("_PR", "CPU0", root=True) + b"\x01" + struct.pack("<I", 0x810) +
                            b"\x06" + pss)
    body = sb + tz + cpu

    header = bytearray(b"SSDT" + struct.pack("<IBB", 36 + len(body), 2, 0) + b"LKXTST" +
                       b"PROBETST" + struct.pack("<I", 1) + b"LKX " + struct.pack("<I", 1))
    table = header + body
    table[9] = (-sum(table)) & 0xFF
    with open(sys.argv[1], "wb") as f:
        f.write(table)


if __name__ == "__main__":
    main()
