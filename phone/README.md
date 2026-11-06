# Phone build (phase 6)

Everything here runs in WSL. Nothing here flashes: flashing is a separate step,
only with the owner's confirmation each time (CLAUDE.md).

| File | What it does |
|---|---|
| `fetch.sh` | Copies the samsung-a5 recipe from pmaports into this folder. Also clones pmbootstrap to `~/lkx-phone`. |
| `device-samsung-a5/`, `kernel/` | That copy. `samsung-a5` is in pmaports' `archived/` folder (no maintainer), so we keep our own. `PMAPORTS-COMMIT` says which commit. |
| `build-kernel.sh` | Builds Nura's kernel (same source, patch and config) with our Buildroot compiler. Needs no root. |
| `bootimg.py` | Reads and writes Android boot images. Checked: rebuilds Nura's boot image byte for byte. |
| `make-boot.sh` | Our kernel plus Nura's initramfs and cmdline: `boot-our-kernel.img`. |

## pmbootstrap

pmbootstrap 3.12.1 is in `~/lkx-phone/pmbootstrap` (from git: it left PyPI) but
stops here without `sudo`. Building Nura's whole image with it needs, once:

```bash
sudo apt install kpartx
```

and it asks for the WSL password whenever it sets up its chroots. Then:

```bash
python3 ~/lkx-phone/pmbootstrap/pmbootstrap.py init
```

`samsung-a5` being archived may hide it from `init`'s device list. Unverified.

## Unverified

The kernel is built with GCC from Buildroot, Nura's with Alpine's. Nura's
rootfs holds modules from its own build, so ours may refuse to load them.

## Getting back to Android

`restore-stock.sh`, run in the Fedora VM. `quick` writes BOOT and USERDATA only,
the partitions Nura is expected to use. `full` adds RECOVERY, SYSTEM, CACHE and
HIDDEN. The firmware's own partition map (`A5ULTE_EUR_OPEN.pit`, 31 entries)
names them. Bootloaders, modem and EFS aren't in the script at all.

`heimdall download-pit` ran cleanly through the Fedora VM. The phone's map (8192 bytes
padded) is byte-identical to the firmware's 4376-byte map, so these partition names
are right. A flash, and this restore, are still untested.

## How Nura boots on the A5 (found with `fastboot getvar all` and the image itself)

- lk2nd's fastboot: `unlocked: yes`; `boot` is only 0xc80000 (12.5 MiB), `userdata` about 12 GiB.
- Nura's `*-samsung-a5.img` is an Android **sparse** image holding a whole disk: an MBR, a boot partition
  (487 MiB: `extlinux/extlinux.conf`, `vmlinuz`, `initramfs`, the device tree, and a 22 MB `boot.img`)
  and the root (2.4 GiB). It goes to `userdata`: `fastboot flash userdata`. lk2nd then boots it by extlinux.
- So Nura's `*-boot.img` (22 MB) does NOT go to the `boot` partition: it can't fit, and `boot` holds lk2nd.
- Our own kernel is therefore tried by putting it on that boot partition (or `fastboot boot`, which writes
  nothing). `make-boot.sh`'s boot image is only useful for the second.
- `fastboot getvar all` prints `wifimacaddr`, `btmacaddr` and `serialno`: filter `serial|imei|mac`.
