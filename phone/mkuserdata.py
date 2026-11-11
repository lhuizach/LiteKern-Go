#!/usr/bin/env python3
"""Puts the pieces of the phone's userdata image together (nothing here touches a phone):

  mkuserdata.py BOOT.ext2 DATA.img OUT.raw

  sector 0            an MBR: partition 1 the boot partition (ext2, what lk2nd's extlinux reads),
                      partition 2 the data disk (type 0xDA, no filesystem the kernel knows)
  sector 2048 ...     the boot partition (BOOT_MB megabytes)
  sector DATA_START   the data disk: our shell's own disk image (an MBR, the apps' ramdisk at
                      sector 4096, then the FAT32 partition from sector 16384). Its first bytes
                      are marked LKGODATA, which the phone's start-up checks for.

DATA_START below is the same number as in linux/overlay/usr/bin/lkx-start."""
import os
import struct
import sys

SECTOR = 512
BOOT_START = 2048
BOOT_MB = 64
DATA_START = 133120                     # = BOOT_START + 64 MiB of sectors; keep in step with lkx-start
MARK = b"LKGODATA"


def entry(boot, ptype, start, sectors):
    return struct.pack("<B3sB3sII", 0x80 if boot else 0, b"\xfe\xff\xff", ptype, b"\xfe\xff\xff", start, sectors)


def main(boot_path, data_path, out_path):
    boot_size = os.path.getsize(boot_path)
    data_size = os.path.getsize(data_path)
    assert boot_size <= BOOT_MB * 2**20, "the boot partition's files are too big for %d MiB" % BOOT_MB
    assert BOOT_START + BOOT_MB * 2048 == DATA_START
    data_sectors = (data_size + SECTOR - 1) // SECTOR
    mbr = bytearray(512)
    mbr[440:444] = b"LKGO"             # the disk's own signature
    mbr[446:462] = entry(True, 0x83, BOOT_START, BOOT_MB * 2048)
    mbr[462:478] = entry(False, 0xDA, DATA_START, data_sectors)
    mbr[510:512] = b"\x55\xaa"
    with open(out_path, "wb") as out:
        out.write(mbr)
        out.seek(BOOT_START * SECTOR)
        with open(boot_path, "rb") as b:
            while True:
                chunk = b.read(1 << 22)
                if not chunk:
                    break
                out.write(chunk)
        out.seek(DATA_START * SECTOR)
        with open(data_path, "rb") as d:
            first = d.read(SECTOR)
            out.write(MARK + first[len(MARK):])   # the marker, over the data disk's unused boot code
            while True:
                chunk = d.read(1 << 22)
                if not chunk:
                    break
                out.write(chunk)
        out.truncate((DATA_START + data_sectors) * SECTOR)
    print("userdata image: %d MiB (boot %d MiB, data %d MiB)" % (
        (DATA_START + data_sectors) * SECTOR // 2**20, BOOT_MB, data_sectors * SECTOR // 2**20))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
