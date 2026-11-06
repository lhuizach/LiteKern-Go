#!/bin/sh
# LiteKern Go, phone: trims the system after Buildroot has installed everything
# (BR2_ROOTFS_POST_BUILD_SCRIPT in linux/buildroot-phone.defconfig). lk2nd leaves
# about 18 MB for the RAM disk (phone/README.md), so what the phone can't use goes.
TARGET_DIR=$1
# udev's names for PCI and USB vendors and devices: 17 MB, and a phone has neither.
rm -rf "$TARGET_DIR/lib/udev/hwdb.bin" "$TARGET_DIR/lib/udev/hwdb.d" "$TARGET_DIR/etc/udev/hwdb.d"
# Documentation and locales, if any package left them.
rm -rf "$TARGET_DIR/usr/share/man" "$TARGET_DIR/usr/share/doc" "$TARGET_DIR/usr/share/info" "$TARGET_DIR/usr/share/locale"
exit 0
