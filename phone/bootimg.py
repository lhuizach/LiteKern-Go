#!/usr/bin/env python3
"""LiteKern Go - Android boot images (header version 0), the format lk2nd loads.

  bootimg.py info BOOT.img                  print its header
  bootimg.py unpack BOOT.img DIR            write DIR/kernel, DIR/ramdisk
  bootimg.py pack LIKE.img OUT.img KERNEL RAMDISK
                                            the same layout and cmdline as
                                            LIKE.img, with this kernel and ramdisk

Only reads and writes files here. Flashing is never done by a tool in this
folder: see CLAUDE.md's safety rules.
"""
import hashlib
import os
import struct
import sys

MAGIC = b"ANDROID!"
HEADER = "<8s10I16s512s32s1024s"
FIELDS = ("magic kernel_size kernel_addr ramdisk_size ramdisk_addr second_size second_addr "
          "tags_addr page_size header_version os_version name cmdline id extra_cmdline").split()


def read_header(data):
    h = dict(zip(FIELDS, struct.unpack_from(HEADER, data)))
    if h["magic"] != MAGIC:
        sys.exit("not an Android boot image")
    if h["header_version"] != 0:
        sys.exit(f"header version {h['header_version']}: only version 0 is handled")
    return h


def pages(n, page):
    return (n + page - 1) // page * page


def parts(data, h):
    page = h["page_size"]
    k = page
    r = k + pages(h["kernel_size"], page)
    s = r + pages(h["ramdisk_size"], page)
    return (data[k:k + h["kernel_size"]], data[r:r + h["ramdisk_size"]],
            data[s:s + h["second_size"]])


def text(b):
    return b.rstrip(b"\0").decode("ascii", "replace")


def info(path):
    data = open(path, "rb").read()
    h = read_header(data)
    print(f"size        {len(data)}")
    base = h["kernel_addr"] & ~0xfffff
    print(f"kernel      {h['kernel_size']} bytes at {h['kernel_addr']:#x}")
    print(f"ramdisk     {h['ramdisk_size']} bytes at {h['ramdisk_addr']:#x}")
    print(f"second      {h['second_size']} bytes at {h['second_addr']:#x}")
    print(f"tags        {h['tags_addr']:#x}")
    print(f"page size   {h['page_size']}")
    print(f"base        about {base:#x}")
    print(f"name        {text(h['name'])!r}")
    print(f"cmdline     {text(h['cmdline'])!r}")


def unpack(path, out):
    data = open(path, "rb").read()
    kernel, ramdisk, second = parts(data, read_header(data))
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "kernel"), "wb").write(kernel)
    open(os.path.join(out, "ramdisk"), "wb").write(ramdisk)
    if second:
        sys.exit("this image has a second stage: not expected on samsung-a5")


def pad(b, page):
    return b + bytes(pages(len(b), page) - len(b))


def pack(like, out, kernel_path, ramdisk_path):
    h = read_header(open(like, "rb").read())
    kernel, ramdisk = open(kernel_path, "rb").read(), open(ramdisk_path, "rb").read()
    h["kernel_size"], h["ramdisk_size"], h["second_size"] = len(kernel), len(ramdisk), 0
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b""):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    h["id"] = sha.digest().ljust(32, b"\0")
    page = h["page_size"]
    header = pad(struct.pack(HEADER, *(h[f] for f in FIELDS)), page)
    with open(out, "wb") as f:
        f.write(header + pad(kernel, page) + pad(ramdisk, page))


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "info":
        info(a[1])
    elif len(a) == 3 and a[0] == "unpack":
        unpack(a[1], a[2])
    elif len(a) == 5 and a[0] == "pack":
        pack(*a[1:])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
