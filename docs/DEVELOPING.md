# Developing LiteKern X

Everything you need to build, test and work on LiteKern X. For what the project is, see the [README](../README.md); for the plan, see [`LiteKernX-Roadmap/`](../LiteKernX-Roadmap/) (the phase files there are the source of truth).

## Setup (Windows + WSL Ubuntu)
Run these once, from PowerShell in the project folder:
```
wsl sudo bash tools/setup-wsl.sh
wsl make check-tools
```
The toolchain is the host's `gcc -m32 -ffreestanding`, GNU `ld` and `nasm`, all in WSL. A proper `i686-elf` cross-compiler is a [Non-Goal](NON-GOALS.md) until host gcc causes real problems.

## Everyday commands
| Command | What it does |
|---|---|
| `wsl make` | Builds `build/litekernx.img`: bootloader, kernel, apps, wallpapers, and a FAT32 partition |
| `wsl make run` | Builds and boots it in a QEMU window |
| `wsl make test` | Runs every test: the VM smoke test, the bootloader tests, the FAT32 tests, the picture decoder tests, the users tests and the kernel tests (about 12 minutes) |
| `wsl make usb` | Builds the images for the real Eee PC into `build/usb/` (see [HARDWARE-TEST.md](HARDWARE-TEST.md)) |
| `wsl make debug` | Like `run`, paused, with a gdb stub on `:1234`; interrupts and resets logged to `build/qemu-debug.log` |
| `wsl make smoke` / `smoke-gui` | Boots a tiny test image, to check the VM itself works |

Build-time choices: the defaults, until the user changes them in Settings. What they choose is saved on the stick, in `/LiteKern/settings.txt` (and `/LiteKern/wallpaper.lkxw` for a picture wallpaper; `kernel/settings.h`), and wins from then on. Rebuilding the image starts it afresh. Delete the `LiteKern` folder on the stick to go back to the defaults:
```
wsl make STYLE=light ACCENT=teal WALLPAPER=dusk SPLASH=log
```
The screen saver (glowing ribbons, `kernel/screensaver.h`) starts after 5 minutes without input; `SCREENSAVER=<seconds>` changes that, and `SCREENSAVER=0` turns it off. To see it straight away: `wsl make run SCREENSAVER=5`.

The keyboard is French AZERTY (`kernel/keymap.c`, shared by the PS/2 driver and the Linux build); `KEYBOARD=qwerty` builds the US layout. `tests/kernel/test-kernel.sh` builds with qwerty, because QEMU types by key position with US names, and checks AZERTY on its own build.

## VirtualBox
There's a second dev VM, `LiteKern X`, in your Windows VirtualBox, with the same Eee PC-like profile as the QEMU VM plus the real panel's **1024×600** mode (QEMU can't do that one). It's driven from WSL by [`vm/vbox.sh`](../vm/vbox.sh):

| Command | What it does |
|---|---|
| `wsl make vbox-create` | Creates and registers the VM (once) |
| `wsl make vbox` | Rebuilds, copies the image to the VM's disk, and boots it in a window. Power the VM off first |
| `wsl make vbox-test` | The same, headless: waits for the desktop, saves `build/vbox/screen.png`, powers off |
| `wsl bash vm/vbox.sh stop\|status\|destroy` | Manage the VM |

The VM's log (COM1) goes to `build/vbox/serial.log`. To run a self-test image instead, or type into the VM during a test:
```
wsl IMAGE=build/test-user/litekernx.img "WAIT_FOR=^selftest: user" bash vm/vbox.sh test
wsl "WAIT_FOR=^kbd: ready" "TYPE=Hi!" "TYPED=kbd: key 0x002 .!." bash vm/vbox.sh test
```

## Logs, screenshots and recordings
- In QEMU, the log (COM1) goes to the terminal and to `build/serial.log`. On the Eee PC, which has no serial port, the **Log** app shows it. **VM timings don't count toward the [boot budget](BOOT-BUDGET.md).**
- [`tests/lib/drive.py`](../tests/lib/drive.py) drives a headless QEMU from a little script (move the pointer, click, type, take screenshots, record MP4s). The README's screenshots were made with it.

## On Linux (the port: `LiteKernX-Roadmap/06-LiteKernX-Linux-Port.md`)
The same desktop as a Linux program, `lkx-shell`: today's `kernel/` UI code with the Linux versions of what it got from the hardware in `platform/` (`platform/platform.h` says what each file does). It opens the kernel build's disk image, so the ramdisk (apps, wallpapers) and the FAT32 partition (settings, users) are the same.

| Command | What it does |
|---|---|
| `wsl make -f platform/Makefile` | Builds `build-linux/lkx-shell` (32-bit, like the EeePC's Linux will be) |
| `wsl make -f platform/Makefile run` | Opens it in a window on the PC (WSLg's X server, spoken to directly: nothing to install) |
| `wsl bash tests/linux/test-linux.sh` | Its tests: headless, scripted like the kernel tests, with screenshots (about 1 minute) |

Options: `--size 1024x768` (the VMs' screen; the default is the EeePC's 1024x600), `--disk IMAGE`, `--keyboard qwerty`, and for scripts `--backend headless --keys "STEPS" --shot FILE.png` (the key language of `tests/lib/qemu_monitor.py`). The apps are Linux programs too, built from the same sources into `build-linux/apps/<name>/<name>` (`LKX_APPS=` points the shell elsewhere); each runs as its own process and talks to the shell over a socket (`platform/kip.h`). `make -f platform/Makefile EXTRA_APPS="tests/apps/crash ..."` builds the tests' misbehaving apps as well.

The USB stick (phase L3): `wsl bash linux/build.sh` builds `build-linux/litekernx-linux.img` with Buildroot (the first time 1–2 hours; then `linux/build.sh image`, minutes), `wsl bash tests/linux/test-image.sh` boots it in QEMU and tests it, and [HARDWARE-TEST-LINUX.md](HARDWARE-TEST-LINUX.md) is the EeePC checklist.

## Where things are
```
LiteKernX-Roadmap/   the plan, phase by phase
boot/                the bootloader (stage 1 MBR, stage 2), boot_info, the BIOS thunk
kernel/              the kernel: memory and paging, interrupts, the driver layer, ring 3
                     apps and their system calls (lkx.c, sys_app.c, user.c), the window
                     system (wm.c), the shell and compositor (desktop.c), graphics
                     (gfx.c, text.c, theme.c), FAT32, ACPI (power-off, reading the tables),
                     the hardware probe (hwprobe.c)
drivers/             hardware drivers behind driver_t: serial, VBE display, PS/2 keyboard
                     and touchpad, RTC, BIOS disk, ATA, the embedded controller (read-only)
apps/                the KERN86 apps (Files, Calculator, Settings, Hardware Probe, Blocksmith); one folder each
sdk/                 what apps are built with (kern86.h and friends)
assets/              logo, icons, cursors, wallpapers, the GUI font
tools/               build-time converters (fonts, icons, wallpapers, the ramdisk) and setup
platform/            the Linux port: lkx-shell's devices, memory, time, event loop and backends
                     (headless, x11); platform/include shadows kernel headers Linux can't use
docs/                this guide and the rest (below)
tests/               bootloader, FAT32, kernel and Linux (tests/linux) tests; test apps; the QEMU helpers
vm/                  the QEMU and VirtualBox dev VMs
```

## More docs
- [Making an app](APPS.md): KERN86 apps, their manifest, the SDK
- [Blocksmith](BLOCKSMITH.md): the block coding app, how to use it and its blocks
- [Testing on the real Eee PC](HARDWARE-TEST.md)
- [Boot protocol](BOOT-PROTOCOL.md) and [boot-time budget](BOOT-BUDGET.md)
- [Asset rules and prompts](ASSET-PROMPTS.md): icons, cursors, wallpapers, the logo
- [Non-Goals](NON-GOALS.md): what LiteKern X deliberately doesn't do
- [Monitor and fan control](MONITOR.md): the design, the safety rules, and each stage (the Hardware Probe first)
