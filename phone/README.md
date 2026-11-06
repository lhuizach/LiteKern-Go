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

## First boot of Nura Phosh v26.06 on the A5 (phase 5 results)

Tried by the owner on the phone itself, 2026-11-06:

| Part | Result |
|---|---|
| Boot: lk2nd, then extlinux, then Nura | works; the first unlock into Phosh took a few minutes (the session's first start) |
| Screen | works, "great" (the panel driver is in Nura's kernel) |
| Wi-Fi | works: networks listed, joined, and the clock then set itself |
| Camera | **not found** ("can't be found"), as the wiki user had reported |
| SIM / mobile data | no SIM in the phone, so untested (the status bar's `?` is that) |
| Bluetooth | an icon shows; untested |

Not yet checked: touch details, speed, brightness slider (no backlight device), sound (by ear)
USB networking (172.16.42.1), SD card.

## What the A5 shows under Nura (read over SSH, 2026-11-06)

Login: `ssh user@<the phone's Wi-Fi address>` with a key (Claude's, in `authorized_keys` as
`litekern-go-claude`; delete that line to revoke). sshd is a systemd service here (`systemctl`).
Nura is **Alpine (musl)**: our Buildroot programs are glibc and won't run on it as they are.

| Part | What the kernel shows |
|---|---|
| Camera | **no camera device**: only `qcom-venus` encoder/decoder in `/dev/video*`, no `/dev/media*`, nothing in the log. `CONFIG_VIDEO_QCOM_CAMSS=m` is built, not loaded; no sensor driver is on. Needs device-tree nodes and a driver for the A5's sensors: kernel work, sensor model not found yet. |
| Brightness | **no `/sys/class/backlight`**: confirmed gap. |
| Battery | works: `rt5033-battery` and `rt5033-charger` (capacity 59% when read). |
| Sound | a card exists (`samsung-a2015`); the codec drivers are loaded; `tfa9895` amp on i2c. Untested by ear. |
| Vibrator | `pwm-vibrator` input device (event6): **haptic feedback is possible**. |
| Keys | `pm8941_pwrkey` (event0, Power), `pm8941_resin` (event1), `GPIO Buttons` (event2), `tm2-touchkey` (event5, the touch keys), headset jack (event7). Codes not read yet. |
| Touch | `MELFAS MMS345` (event4); a Hall sensor (cover) on event3. |
| GPU | `/dev/dri/card0` + `renderD128`, module `msm`; whether the Adreno is drawing (or the CPU) is not known: dmesg needs root. |
| Other i2c | NFC `s3fwrn5`, USB-cable detector `sm5502-muic`, accelerometer+compass `bmc150`. |
| USB | a gadget controller (`ci_hdrc.0`) exists; no network device has shown up on the PC yet. |
| Memory / disk | 1.8 GB RAM (about 590 MB used at the desktop), 4 cores; root grew to 11 GB by itself. |

### The keys (read over SSH, all six pressed)

| Key | Device | Code |
|---|---|---|
| Recents (left touch key) | `tm2-touchkey` (event5) | 139 `KEY_MENU` |
| Back (right touch key) | `tm2-touchkey` (event5) | 158 `KEY_BACK` |
| Home (physical) | `GPIO Buttons` (event2) | 172 `KEY_HOMEPAGE` |
| Volume Up | `GPIO Buttons` (event2) | 115 |
| Volume Down | `pm8941_resin` (event1) | 114 |
| Power | `pm8941_pwrkey` (event0) | 116 |

Recipe: `/dev/input/event*` is `root:input`, so `sudo addgroup user input` once on the phone.

## Our kernel on the phone (`fastboot boot`, 2026-11-06)

`phone/build-kernel.sh` then `phone/make-boot.sh` gave `boot-our-kernel.img` (Nura's boot image with our kernel),
sent with `fastboot boot` from lk2nd's menu. `sudo reboot bootloader` only restarted into Nura; the owner then
reached the menu by the power-off and Volume Down way I suggested (their report, not checked). RAM only: nothing is written; a plain restart returns to
Nura's own kernel.

- It boots: `uname -v` shows our build (`#2 ... lhuir@GamingPC ... Buildroot gcc 14.3.0`).
- `tainted` is 0 and 102 modules load from Nura's rootfs: the same version, config and vermagic.
- Seen working: Wi-Fi (reached over SSH), all eight input devices, battery, the sound card, `/dev/dri`.
- Not yet checked under it: the desktop on screen, touch, the GPU.
- USB tip: VirtualBox sometimes holds the phone's device as "Captured" without giving it to the VM
  (fastboot says `waiting for any device`): unplug and replug the cable, then attach again.

## Our own system on the phone, RAM-booted (stage 2, nothing flashed)

```
bash phone/build-kernel.sh                        # the kernel and its modules (Nura's source, patch, config)
LKX_ARCH=aarch64 LKX_PHONE=1 bash linux/build.sh  # the system: output-go-phone/images/rootfs.cpio.gz (29 MB)
bash phone/make-ramboot.sh                        # ~/lkx-phone/out/boot-litekern-go.img
```

Then `fastboot boot boot-litekern-go.img` from lk2nd's menu (each `fastboot boot` needs the owner's yes).

- The system is busybox init + eudev (loads the drivers by modalias: `80-drivers.rules`, kmod with zstd for the
  `.ko.zst` modules) + our shell and apps + dropbear (root, key from `phone/authorized_keys`, not in git) + the
  firmware copied from a running Nura (`~/lkx-phone/firmware`). glibc with kernel headers 6.12.
- The shell's disk is the file `/usr/share/litekern/disk.img` (the phone has no stick); its log is `/tmp/lkx.log`.
- The kernel's text console is the screen first (`console=tty0`), so a fault shows on the panel.
- Not in this system yet: Wi-Fi (needs the firmware loader Nura has: the wcnss files come from the modem
  partition), so no ssh until that, and nothing runs the GPU.
