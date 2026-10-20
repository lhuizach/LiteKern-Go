# LiteKern X on Linux — the port
**Started 2026-10-19, on the user's decision.** This repository is a copy of `C:\Users\lhuir\LiteKern X` (the own-kernel version, commit 5c4e130, kept as the git remote `kernel-version`). That one stays as it is; this one becomes LiteKern X running on the Linux kernel.

**Why:** the user wants Linux apps, Wi-Fi and a browser that opens modern HTTPS sites on the EeePC 1000HE. On the own kernel that needs a network stack, TLS, a Wi-Fi driver and floating point in apps first (months); on Linux they come with the kernel.

**The rule for this port (the user's):** before any phase starts, ask the user, with:
- the estimated time,
- the size of the phase,
- what the user has to do for it (run a command, flash the USB stick, test on the EeePC, send photos of the Log).

Then wait for a yes. One phase at a time, each finished and tested before the next.

**What stays the same: all of today's UX.** The desktop, top bar, dock, app menu, windows and their animations, the lock screen and its curtain, users and passwords, Log Out, the screen saver, wallpapers, themes and accents, the Log app, the power menu, the AZERTY keyboard, and the apps (Files, Calculator, Settings, Blocksmith, Hardware Probe). They should look and behave as they do now; Linux is underneath, not on show.

---

## How it fits together (decided 2026-10-19)
- **The kernel is Linux** (32-bit x86, i686: the Atom N280 has no 64-bit mode), built with **Buildroot** into a small image for the USB stick. No ready-made distro: few still support 32-bit, and Buildroot keeps it small and fast to boot.
- **The desktop is one Linux program** (`lkx-shell`), made from today's `kernel/` UI code: `desktop.c`, `wm.c`, `widget.c`, `theme.c`, `gfx.c`, `text.c`, `font.c`, `lockscreen.c`, `wallpaper.c`, `screensaver.c`, `settings.c`, `users.c`, `splash.c`, `cursor.c`, `icon.c`, `deflate.c`, `inflate.c`, `sha256.c`. They depend on a few small kernel interfaces (memory pages, logging, timers, the screen, input devices, the ramdisk, the clock, storage, power); those headers stay and get **Linux versions** in a new `platform/` folder. The UI code itself changes as little as possible.
- **Two backends for the screen and input:**
  - `sdl`: a window on the development PC (WSL's own GUI), for building and testing quickly;
  - `drm`: the real thing on the EeePC (Linux DRM/KMS for the screen, evdev for the keyboard and touchpad).
- **Apps stay separate programs** using the same SDK (`sdk/kern86.h`): `k86_*` calls become messages to the shell over a local socket, and each window's pixels are shared memory. An app that crashes still can't take the desktop down, and the kernel-side checks (`kernel/sys_app.c`: paths, users, privacy) stay in the shell.
- **Logs:** the shell's log lines are the same as today, so the Log app and the QEMU tests keep working; the Log app also shows Linux's own messages.
- **Dropped (Linux does them):** `boot/`, the BIOS thunk, VBE and its 1024x600 patch, GDT/IDT/paging/IRQs, `pmm`/`vmm`, ring 3 and `lkx` loading, `drivers/`, `fat32.c`, `bios_disk.c`, ACPI and AML. They stay in the `kernel-version` repository.

## Still to decide (each at the phase that needs it)
- Where user files live: a FAT32 partition on the stick (readable on a Windows PC, as today), or Linux's own filesystem (permissions, but Windows can't read it). Phase L4.
- Linux users for LiteKern X's users, or LiteKern X's own `users.txt` as today. Phase L4.
- The browser's TLS library (OpenSSL, or mbedTLS, which is smaller). Phase L6.
- Linux apps from outside (needs the shell to become a Wayland compositor). Phase L8.

---

## L0. Setup — done 2026-10-19
- [x] Copy of the code, with its history (`C:\Users\lhuir\LiteKern-X-Linux`)
- [x] This roadmap

## L1. The desktop as a Linux program, in a window on the PC — **done 2026-10-19** (the user was away: no SDL2 needed in the end)
**Estimate:** 3–5 hours of work. **Size:** about 1,500–2,500 new lines (the `platform/` layer and the SDL backend), small edits to the UI code.
**You:** run one command in WSL to install SDL2 (it needs your password); look at the screenshots; if you like, open it on your PC.
**As built:** about 1,850 new lines in `platform/` and `tests/linux/`, and **no change to the UI code** beyond two splits that the kernel build shares: the keyboard layouts (`kernel/keymap.c`, out of `drivers/kbd.c`) and the app list (`kernel/appreg.c`, out of `kernel/lkx.c`). **Changed from the plan:** no SDL2. The window backend speaks the X11 protocol to WSLg's X server itself, so nothing has to be installed (and nothing needed the user's password); it's checked by reading pixels back from the X server (`LKX_X11_VERIFY=1`). The headless backend runs the tests' own key language and writes PNG screenshots. Built 32-bit (`-m32`), like the EeePC's Linux will be, so the kernel code keeps its 32-bit addresses. The shell opens the kernel build's disk image as its "boot disk": the ramdisk and the FAT32 code work unchanged.
- [x] `platform/`: memory (mmap), logging (stdout, like the serial port), time and the clock, the disk image as `boot0`, the screen, keyboard and pointer as `fb0`, `kbd0`, `mouse0`, `rtc0` for the kernel's driver layer
- [x] ~~The SDL backend~~ the x11 backend: a 1024x600 window, keyboard (AZERTY, by key position), mouse; and the headless backend for tests
- [x] The desktop, top bar, dock, app menu, windows, lock screen, screen saver, wallpapers, themes, settings and users, the Log app; no other apps yet (their icons show; opening one says L2)
- [x] Tests: `tests/linux/test-linux.sh` (12: start-up, app menu, Log and its close button, the power menu's Lock, Super+L and the big clock, the login screen with a wrong then right password, a user without a password, AZERTY, the screen saver, and the kernel's wm, gfx and widgets self-tests, unchanged); the host-side tests (users, image) pass
- [ ] Still to do when the user is back: open the window (`wsl make -f platform/Makefile run`) and try it by hand: the keyboard and mouse through X haven't been used by a person yet

## L2. The apps on Linux — **done 2026-10-19** (the user said go ahead while away)
**Estimate:** 4–6 hours. **Size:** about 1,500–2,500 lines (`libk86` for apps, the app server in the shell).
**You:** try the apps in the window, look at screenshots.
**As built:** about 1,400 lines (`platform/kip.h`, `libk86.c`, `lkx.c`, `hwprobe.c`, tests). **The apps' sources are unchanged**; `sdk/kern86.h` sends `k86_call()` to `platform/libk86.c` when built for Linux, and **`kernel/sys_app.c` runs unchanged in the shell, with every check in it**: the app sends the bytes behind its pointers (each call's arguments are described once, in `platform/kip.h`), the shell hands `sys_app.c` pointers to its copies, and the outputs go back. A pointer the app can't touch itself (`process_vm_readv` on itself says so, without faulting) reaches `sys_app.c` as one its checks refuse, so the answers come in the kernel's order: the bad-calls test app gets the same 16 answers as on the kernel. Window pixels are shared memory (the shell's page allocations are memory files, `memfd`). Apps are separate Linux processes (fork + exec, a socket on fd 3); a crash is a signal, logged as the kernel names it; the 10 s watchdog is the kernel's.
- [x] `libk86`: `sdk/kern86.h` over a socket and shared memory
- [x] The app server: today's `sys_app.c` checks, windows, input, files, users
- [x] Files, Calculator, Settings, Blocksmith; the Hardware Probe reads Linux's `/proc` and `/sys` instead of the hardware (the user asked whether it's still needed: its driver-hunting job is gone on Linux, kept as a cheap system-info page, easy to drop)
- [x] An app crashing or hanging doesn't take the desktop down (the `crash`/`hang`/`badcalls` test apps)
- [x] Tests: `tests/linux/test-linux.sh` now 20 checks (Calculator, Files and closing it, Settings saved and kept, a user added, Blocksmith run and save, the Probe, Files opened and closed 15 times, the misbehaving apps)
- [ ] Still to do when the user is back: try the apps in the window by hand

## L3. A bootable USB stick — **done in QEMU 2026-10-19; waiting for the EeePC**
**Estimate:** 5–8 hours, plus 1–2 hours for Buildroot's first build. **Size:** Buildroot and kernel configuration, about 800–1,500 lines (the DRM/evdev backend, start-up).
**You:** one WSL command to install Buildroot's tools; flash the stick; boot the EeePC; photos of the screen and the Log (display, touchpad, keyboard, how long boot takes).
**As built:** `linux/` (Buildroot 2026.02.3 LTS as an external tree: `buildroot.defconfig`, `kernel.config`, the overlay with `inittab` and `lkx-start`; `build.sh` builds it all in `~/lkx-buildroot`), `platform/drm.c` (DRM/KMS with a dumb buffer and DIRTYFB, evdev with hotplug through inotify; no libraries). **The stick keeps the kernel version's layout:** the ramdisk at sector 1024 and the FAT32 partition at 8192; SYSLINUX's MBR boots the FAT32 partition, whose `/syslinux` holds Linux and the system (an initramfs: it all runs from RAM). `lkx-start` finds the stick by its ramdisk and lkx-shell opens it as its disk, as on the PC: the FAT32 code, settings, users and files carry over unchanged. The touchpad is a plain PS/2 mouse (`psmouse.proto=exps`), as the kernel version drives it. Restart and Shut Down are Linux's (`reboot()`), done here rather than in L4. Two WSL snags fixed in `build.sh`: Windows folders in PATH, and Ubuntu 26.04's Rust coreutils (the GNU ones, still installed as `gnu*`, go first).
- [x] Buildroot for i686 (tuned for the Atom), a Linux kernel set up for the 1000HE (Intel graphics, the touchpad, the keyboard, USB, the Atheros Ethernet `atl1e` and Wi-Fi `ath9k`, the Eee PC's ACPI bits)
- [x] The DRM/KMS and evdev backend
- [x] Boots straight into the desktop, with the splash
- [x] The QEMU tests run against the Linux image (`tests/linux/test-image.sh`: boot, Calculator with QEMU's mouse and keyboard, Super+L, Shut Down powers off)
- [ ] On the EeePC (`docs/HARDWARE-TEST-LINUX.md`): the panel at 1024x600, the touchpad and keyboard, a USB mouse at full speed, boot time

## L4. Everything the kernel version does
**Estimate:** 4–6 hours. **Size:** about 1,000–1,500 lines.
**You:** flash; go through a checklist on the EeePC; photos.
- [ ] The power menu: Restart and Shut Down through Linux
- [ ] Users, passwords, the lock screen, Log Out; each user's settings and home folder
- [ ] Storage: the decision above; Files sees the stick and the internal disk
- [ ] Settings kept across restarts; the Log shows Linux's messages
- [ ] Free from Linux: USB mice and keyboards at full speed (the BIOS slowdown is gone), battery level, brightness keys

## L5. Networking — **done in QEMU 2026-10-20; waiting for the EeePC**
**Estimate:** 4–6 hours. **Size:** about 1,000–1,500 lines.
**You:** flash; join your Wi-Fi from Settings; photos.
**As built:** about 1,250 lines. `platform/net.c` runs the network from lkx-shell's own loop, no daemon of ours: Ethernet with busybox's `udhcpc`; Wi-Fi through `wpa_supplicant`'s control socket (scan, join WPA2/WPA3/open, its events: a refused password), the radio unblocked (rfkill); remembered networks on the stick (`/LiteKern/wifi.txt`), given back to `wpa_supplicant` at start so it rejoins by itself; a network is only remembered once it has joined. Four new calls (`SYS_NET_STATUS/SCAN/JOIN/FORGET`, `kernel/net.h`): the kernel version answers "no network hardware" (`kernel/net.c`), so its Settings and top bar look as before. Also from this phase: **writes to the stick are made at once** (`fdatasync`): Linux held them for up to half a minute, so a setting saved just before a power cut was lost. **The user asked for:** Settings' header shows the current tab (a soft background: `wm_select_button`, in the kernel version too). **Changed from the plan:** the clock isn't set from the network yet: Linux would then show UTC, and LiteKern X has no time zone setting (the EeePC's own clock is already right); it waits for one.
- [x] Ethernet and Wi-Fi, DHCP, DNS (`udhcpc` writes `/etc/resolv.conf`); ~~the clock set from the network~~ (above)
- [x] Settings → Network: scan, join with a password (asked again if refused), forget; its tab only with network hardware
- [x] A network icon in the top bar: Wi-Fi's arcs lit by the signal, or Ethernet; dimmed when not connected; a click opens Settings
- [x] Tests (`tests/linux/test-image.sh`, 7 now): Ethernet's address and icon; simulated Wi-Fi (`mac80211_hwsim`, an access point started by `lkx-start` only when the test's command line asks): a wrong password refused, the right one joining, an address, and the network rejoined by itself after a restart
- [ ] On the EeePC: your Wi-Fi from Settings → Network; the Ethernet port with a cable

## L6. The browser
**Estimate:** 8–12 hours. **Size:** about 2,000–3,000 lines (a LiteKern X front end for NetSurf), plus build work.
**You:** flash; try a list of sites; photos.
- [ ] NetSurf (a lightweight C browser) with a LiteKern X front end: address bar, back, forward, reload, links, images, scrolling
- [ ] HTTPS, with the usual root certificates
- [ ] Downloads go to your home folder
- [ ] Expect: Wikipedia, docs, blogs, news, forums. Not expected: YouTube, Google Docs, Discord (they need a full Chromium or Firefox)

## L7. JavaScript in the browser
**Estimate:** 10–20+ hours; uncertain until L6 is done. **Size:** large.
**You:** test sites; photos.
- [ ] QuickJS-ng (the maintained QuickJS fork) joined to NetSurf's DOM, in place of its small Duktape engine
- [ ] Decide after L6 whether it's worth it

## L8. Later (each its own phase, asked first)
- [ ] Linux apps from outside, in LiteKern X windows (the shell as a Wayland compositor)
- [ ] Sound, suspend, the lid, an external display
- [ ] The fan and temperature monitor (`docs/MONITOR.md`) on Linux's `hwmon`
