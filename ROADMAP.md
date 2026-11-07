# LiteKern Go — roadmap

The order things get built in, from the VM to the phone. Each phase ends with
something that can be checked. `[x]` done, `[ ]` not yet.

The UI's own build order is in [UI-PLAN.md](UI-PLAN.md) and is phase 2 here.
Safety rules for flashing are in [CLAUDE.md](CLAUDE.md) and apply to every
phase that touches the phone.

**v1 is done when** the phone boots to the LiteKern Go GUI, takes touch input
and joins Wi-Fi.

## Two tracks

The phone track is blocked on the admin code (installers). The VM track
isn't, so most of the OS gets built there first.

| Track | Phases | Blocked on |
|---|---|---|
| VM (build the OS) | 1, 2, 3, 4 | nothing |
| Phone (hardware) | 0, 5, 6, 7 | admin code, then stock firmware |
| Both | 8, 9 | phases 2 and 7 |

## Phase 0 — Prep for the phone

- [x] Phone: factory reset, charged, download mode tested
- [x] Downloaded and scanned: Heimdall 1.4.0, platform-tools, lk2nd-msm8916.img,
      Nura v26.06 samsung-a5 Phosh images, Frija
- [ ] Admin code
- [ ] Install Visual C++ 2012 x86, .NET 8 Desktop Runtime x86, Zadig driver
- [x] Stock BTU firmware `A500FUXXU1CSB1` downloaded (from samfw.com: Frija found nothing) and checked
- [x] `heimdall detect` sees the phone in download mode (from the Fedora 44 VM, USB passed through: no Zadig needed. Attach it from VirtualBox's Devices > USB)

**Done when:** the stock firmware is saved and Heimdall sees the phone.

## Phase 1 — VM base

- [x] VirtualBox VM sized like the phone (`vm/create-vm.ps1`)
- [x] LiteKern X Linux copied in with its history
- [x] 64-bit port, Linux test suite passes (23/23)
- [x] x86_64 Buildroot image boots in the VM at 720x1280
- [x] Plans: UI-PLAN.md, this roadmap
- [x] GitHub repo (private): github.com/lhuizach/LiteKern-Go
- [x] VM graphics: switch to VBoxSVGA (silence the vmwgfx warning)
- [x] Ethernet "no cable" in the VM (it was only the link coming up a moment late)
- [x] Rename `litekernx-linux.img` and other LiteKern X leftovers
- [x] Remove Eee PC parts from the build (drivers, Hardware Probe)
- [x] 32-bit dropped: 64-bit only (x86_64 VMs, aarch64 phone). The i386 kernel, bootloader and `.lkx` build are gone from this repo (LiteKern X keeps its own)

**Done when:** a clean build boots in the VM with no LiteKern X / Eee PC
leftovers and no driver warnings.

## Phase 2 — Phone shell (in the VM)

The steps from UI-PLAN.md, in order. After 2.5 it looks like a phone.

- [x] 2.1 Theme scale (2x sizes). The desktop-era click tests (12 of 22) fail on the new
      positions: they get rewritten as each phone screen replaces the desktop one
- [x] 2.2 Key map: Back, Home, Recent apps, Power, volume (VM keys: Esc, Home,
      F1, F12, PgUp/PgDn)
- [x] 2.3 Full-screen window manager, one app visible. Not yet: background apps
      paused (stopping their processes would trip the hang watchdog), and closing an
      app (Back in 2.6, the switcher in 2.10)
- [x] 2.4 Home screen: icon pages and All apps, no dock. Not yet: dragging icons
      around, sharper icons (48 px ones scaled up), portrait wallpapers
- [x] 2.5 Status bar
- [x] 2.6 Back: apps say when they have something for it (SYS_WINDOW_BACK), else it
      closes them. Files, Blocksmith and Settings' Users dialogs use it. Not yet: a
      single tap opens a folder in Files (it wants a double tap or Enter)
- [x] 2.7 Touch rules, part 1: lists open on a single tap (on lift) and scroll
      when dragged, a row at a time. Not yet: momentum after a flick, smooth
      (pixel) scrolling, long press (nothing uses it yet), scrolling outside lists
- [x] 2.8 On-screen keyboard: letters, 123 page, Shift and caps lock, Backspace
      repeat, key preview; the app shrinks above it. Not yet: auto-capitals, a number
      pad for number fields, a slide-in animation
- [x] 2.9 Quick settings and notifications: pulled down from the status bar (or Home
      held); Wi-Fi, Dark mode, Do not disturb and Lock tiles, brightness, Settings;
      notifications from apps (SYS_NOTIFY), in the panel and the status bar. Not yet:
      the Wi-Fi tile tried on real Wi-Fi, volume, the brightness slider on a screen
      with a backlight
- [x] 2.10 App switcher: cards of the open apps, tap to go, swipe up to close, Close
      all. Not yet: animations in and out, live pictures (they're taken on opening)
- [x] 2.11 Lock screen: swipe up to unlock, PIN pad (a password is the PIN; wrong
      ones shake and wait), notification titles under the clock, Power held for its
      power menu; one user, no picker. Not yet: setting a PIN (Settings, phase 3), the
      battery while charging, the screen going off on Power (needs the phone)
- [x] 2.12 Power menu (a sheet in the middle: Power off, Restart, Lock), volume pop-up
      (right edge, 2 s, held keeps changing), screenshots (Power + Volume Down, PNG in
      Pictures/Screenshots, a toast). Not yet: real sound (the level is only kept), dragging
      the volume slider, screenshots of the lock screen, keeping the volume across restarts

**Done when:** in the VM, the shell can be used with only the mouse (as a
finger) and the six stand-in keys, and each step has a test in `tests/linux/`.

## Phase 3 — Apps (in the VM)

- [x] Settings reworked for portrait: a list of pages (Wi-Fi, Display, Lock Screen,
      Battery, Date and Time, About Phone), back arrow or Back to return; Display
      (brightness, style, accent, background, screen saver) and Lock Screen (Lock Now,
      Set/Change/Remove PIN, clock, background) scroll with a drag; PIN dialogs open
      the keyboard on its numbers. Users page hidden (one user). Not yet: Sound (no
      audio), Home screen (clock widget on/off), Storage
- [x] Files reworked for portrait: full-width list, New Folder and New File along
      the bottom, a long press for Open, Rename and Delete (a sheet; the list widget
      has long press now), text dragged to scroll, pictures fitted with Set as
      Wallpaper / Set on Lock Screen under them and a swipe to the next one. Not yet:
      thumbnails, copy and move, sharing
- [x] Calculator reworked for portrait: a big display on the top part, round keys
      filling the rest (the thumb's end of the screen), a Del key, +/- by the 0.
      Not yet: a history of past sums, a scientific mode
- [x] Blocksmith: stays a PC app, not reworked for the phone (the owner's call)
- [x] Clock: alarms (daily, a switch each, kept in Clock/alarms.txt; Snooze 5 min), a
      timer typed on a keypad, a stopwatch with laps; a notification when one goes
      off. New app call SYS_TIME (the local time). Not yet: sound (no audio), alarms
      while Clock is closed (no background service), choosing days
- [x] Gallery: a grid of the pictures in Pictures (and its Screenshots, Camera) and
      Downloads, newest first, thumbnails made in the background; it scrolls by the
      pixel and glides after a flick; a tap opens one, a swipe goes to the next;
      Wallpaper, Lock Screen, Delete. Decoding is Files'. Not yet: albums, sharing,
      zooming in on a picture, videos
- [x] Boot screen: the new G logo (tools/make-logo.py) on black, "Powered by Linux" with Tux,
      fading in and cross-fading into the first screen; no Linux or bootloader text
      (the VM boots the kernel from EFI; messages on serial only). The ramdisk moved
      to sector 4096 to make room. Not yet: the phone's own boot (phase 6)
- [x] Motion pass: lists scroll by the pixel and glide on after a flick, slowing
      down (the list widget: Files, Settings' lists; Settings' Display and Lock Screen
      pages, Gallery's grid too); the app switcher fades in with its cards rising, the
      power menu and toasts fade and rise, the volume pop-up slides in from the edge,
      the keyboard slides up and down. Not yet: dialogs and Files' sheet (app-drawn),
      tab and page changes sliding, the switcher's cards following a swipe-up
- [x] Task Manager: the phone's memory (a bar: apps, system, free), the running apps
      with their icons, memory, CPU and whether in front, refreshed every second;
      Stop ends one after asking. New app calls SYS_TASKS, SYS_TASK_STOP, SYS_TASK_ICON
- [x] Dialogs and Files' long-press sheet fade and rise in (wg_animate); the wm and
      widgets self-tests rewritten for the phone (no test fails now)
- [ ] Pick native Linux games with the brother, check they build for aarch64

**Done when:** the v1 apps work in the VM.

## Phase 4 — ARM build (QEMU)

- [x] Buildroot config for aarch64 (Cortex-A53): linux/buildroot-aarch64.defconfig,
      linux/kernel-aarch64.config; `LKX_ARCH=aarch64 bash linux/build.sh`
- [x] Build the shell and apps for aarch64 (x86-only bits made portable: the CPU's
      pause, the TSC, the panic's cli)
- [x] Boot in `qemu-system-aarch64` (Buildroot builds it: no sudo) with a virtual
      touchscreen: `bash linux/vm.sh qemu-arm`; the home screen ~10 s after start
- [x] Run the test suite there, fix ARM-only bugs: `LKX_ARCH=aarch64 bash
      tests/linux/test-image.sh`, all 11 pass (none found). The headless suite runs
      on the PC (x86) only

**Done when:** the aarch64 image boots to the phone shell in QEMU and the
tests pass.

## Phase 5 — First boot on the phone (stock Nura)

Needs phase 0. Every flash needs the owner's OK in chat, one change per flash.

- [x] Flash lk2nd with Heimdall, check it boots to lk2nd (done: from the Fedora VM, `--BOOT`, `--no-reboot`; it boots to its fastboot menu: lk2nd-msm8916 23.1, A5U SM-A500FU, ARM64 available)
- [x] Nura Phosh flashed (one flash: `fastboot flash userdata` of the sparse image, 8 pieces, 193 s) and it boots on the phone
- [ ] SSH in over USB (172.16.42.1)
- [ ] Test and write down: screen, touch, Wi-Fi, USB, storage, SD card, camera,
      brightness, vibration, battery reading, sleep/wake
- [x] The real key codes for Back, Home, Recent apps, Power, volume (read over SSH: all match our key map),
      and whether the touch-key backlights work
- [ ] Check whether modem/EFS data is still intact
- [ ] Keep these images as the first known-good set

**Done when:** a written list of what works on the A5 under mainline Linux.
This decides Camera, brightness, vibration and alarms in UI-PLAN.md.

## Phase 6 — Build environment for the phone

- [x] `.wslconfig` written (memory=12GB, swap=16GB); applies after WSL restarts
- [x] `pmbootstrap` 3.12.1 cloned to `~/lkx-phone`; samsung-a5 recipe kept in `phone/` (archived upstream)
- [ ] `sudo apt install kpartx`, then `pmbootstrap init` (needs the WSL password)
- [x] Build the Nura v26.06 kernel ourselves, no root (`phone/build-kernel.sh`): same version, device tree byte-identical, config differs only in compiler lines
- [x] `phone/make-boot.sh`: our kernel in Nura's boot image, `boot-our-kernel.img` (not flashed)
- [ ] Build the whole Nura image with pmbootstrap
- [x] Boot our build of the kernel on the phone: `fastboot boot` (nothing written) with Nura's userland: our kernel, tainted 0, 102 modules, all input devices, Wi-Fi, battery, sound card, DRM
- [x] Real root filesystem: ext4, the third disk partition, in the x86 and ARM VMs (not the phone yet)

**Done when:** we can rebuild the phone's kernel and boot it.

## Phase 7 — Our own OS on the phone

- [ ] Our initramfs and init on the Nura kernel
- [ ] A shell (text) on the screen
- [ ] Touch input read through evdev, including multitouch (ABS_MT)
- [ ] The aarch64 LiteKern Go image from phase 4 on the phone
- [ ] Key map updated with the real codes from phase 5

**Done when:** the phone boots to the LiteKern Go shell and takes touch.

## Phase 8 — v1: hardware basics in the GUI

- [x] Wi-Fi: scan, join, from Settings (remembering needs a real flash: the RAM boot forgets)
- [ ] Battery and charging in the status bar
- [ ] Brightness (if the driver works)
- [ ] Sleep: screen off with Power, wake with Power/Home
- [ ] Storage and SD card in Files
- [ ] USB: SSH for development, files later

**Done when:** v1: boots to the GUI, takes touch, joins Wi-Fi.

## Phase 9 — Polish and extras

- [ ] The brother's extras (list not written yet)
- [ ] Camera app (if the camera works)
- [ ] Games
- [ ] Battery life and speed tuning on the Snapdragon 410
- [ ] Landscape for games and photos (accelerometer)
- [ ] Later, maybe: Bluetooth, audio playback, calls and mobile data (hardest)

## Open decisions

From CLAUDE.md and UI-PLAN.md:

- The brother's extras list
- Whether no APK games and maybe no camera are OK with him
- PIN lock or swipe only
- Which native games
- Admin code for the installs

## WebKit (in progress)

- [ ] WPE WebKit 2.50 with a C++ toolchain: `LKX_WEB=1 linux/build.sh`, `linux/buildroot-web.fragment`
- [ ] `lkx-web`: runs it in a LiteKern window, replacing NetSurf
- [ ] Runs r3lay (the owner's Supabase app) and modern sites in the VM
- [ ] `freedreno` for the phone's GPU, tested on the phone

## The new look (in progress)

From `docs/mockups/shell-redesign.src.html`: liquid glass on Adwaita's structure.

- [x] `gfx_glass`, a frosted wallpaper, and the home screen, dock and all-apps drawer
- [x] Lock screen, PIN pad, quick settings, power menu, Recent apps
- [x] Shared parts: theme colours, pill buttons, boxed lists, dialogs, the header capsule
- [x] Clock's floating tabs; Files without its repeated title
- [ ] Settings: icons on rows, its pages; large titles
- [ ] Calculator, Gallery, Task Manager, Log, Blocksmith, Web: their own screens
- [ ] App zoom out of the icon; staggered arrivals
- [ ] Glass on the lock screen's own power menu
