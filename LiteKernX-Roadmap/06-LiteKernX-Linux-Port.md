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

## L2. The apps on Linux
**Estimate:** 4–6 hours. **Size:** about 1,500–2,500 lines (`libk86` for apps, the app server in the shell).
**You:** try the apps in the window, look at screenshots.
- [ ] `libk86`: `sdk/kern86.h` over a socket and shared memory
- [ ] The app server: today's `sys_app.c` checks, windows, input, files, users
- [ ] Files, Calculator, Settings, Blocksmith; the Hardware Probe reads Linux's `/sys` instead of the hardware
- [ ] An app crashing or hanging doesn't take the desktop down (the `crash`/`hang` test apps)

## L3. A bootable USB stick
**Estimate:** 5–8 hours, plus 1–2 hours for Buildroot's first build. **Size:** Buildroot and kernel configuration, about 800–1,500 lines (the DRM/evdev backend, start-up).
**You:** one WSL command to install Buildroot's tools; flash the stick; boot the EeePC; photos of the screen and the Log (display, touchpad, keyboard, how long boot takes).
- [ ] Buildroot for i686, a Linux kernel set up for the 1000HE (Intel graphics, the touchpad, the keyboard, USB, the Atheros Ethernet `atl1e` and Wi-Fi `ath9k`)
- [ ] The DRM/KMS and evdev backend
- [ ] Boots straight into the desktop, with the splash
- [ ] The QEMU tests run against the Linux image

## L4. Everything the kernel version does
**Estimate:** 4–6 hours. **Size:** about 1,000–1,500 lines.
**You:** flash; go through a checklist on the EeePC; photos.
- [ ] The power menu: Restart and Shut Down through Linux
- [ ] Users, passwords, the lock screen, Log Out; each user's settings and home folder
- [ ] Storage: the decision above; Files sees the stick and the internal disk
- [ ] Settings kept across restarts; the Log shows Linux's messages
- [ ] Free from Linux: USB mice and keyboards at full speed (the BIOS slowdown is gone), battery level, brightness keys

## L5. Networking
**Estimate:** 4–6 hours. **Size:** about 1,000–1,500 lines.
**You:** flash; join your Wi-Fi from Settings; photos.
- [ ] Ethernet and Wi-Fi, DHCP, DNS, the clock set from the network
- [ ] Settings → Network: scan, join with a password, forget
- [ ] A network icon in the top bar

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
