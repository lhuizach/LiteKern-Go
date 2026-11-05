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

Not run yet: `heimdall download-pit` is the first test, because it does a full
Heimdall session and writes nothing.
