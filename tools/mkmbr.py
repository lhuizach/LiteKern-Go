#!/usr/bin/env python3
"""LiteKern Go - build step: the disk image's first sector, its partition table.

Usage: python3 tools/mkmbr.py OUT.bin RESERVED_SECTORS FAT_START FAT_SECTORS

No boot code: Linux boots from SYSLINUX or EFI (linux/build.sh), or from the
phone's own boot image, and SYSLINUX's MBR is put in by linux/build.sh.
Partition 1 holds the ramdisk, partition 2 the FAT32 files (none if
FAT_SECTORS is 0). lkx-shell finds them by these entries.
"""
import struct
import sys

CHS_LBA = b"\xfe\xff\xff"       # the "use LBA" marker (1023/254/63)
SIGNATURE = 0x20584b4c          # "LKX "


def entry(active, kind, first, sectors):
    return bytes([active]) + CHS_LBA + bytes([kind]) + CHS_LBA + struct.pack("<II", first, sectors)


def main():
    if len(sys.argv) != 5:
        sys.exit("usage: mkmbr.py OUT.bin RESERVED_SECTORS FAT_START FAT_SECTORS")
    out = sys.argv[1]
    reserved, fat_start, fat_sectors = (int(a) for a in sys.argv[2:])
    table = entry(0x80, 0x7f, 1, reserved - 1)
    table += entry(0, 0x0c, fat_start, fat_sectors) if fat_sectors else bytes(16)
    table += bytes(32)
    sector = bytes(440) + struct.pack("<IH", SIGNATURE, 0) + table + b"\x55\xaa"
    assert len(sector) == 512
    with open(out, "wb") as f:
        f.write(sector)


if __name__ == "__main__":
    main()
