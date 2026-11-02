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
- [ ] Download the stock BTU firmware with Frija (the way back)
- [ ] `heimdall detect` sees the phone in download mode

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
- [ ] Rerun the 32-bit test suite once, then decide whether to keep 32-bit

**Done when:** a clean build boots in the VM with no LiteKern X / Eee PC
leftovers and no driver warnings.

## Phase 2 — Phone shell (in the VM)

The steps from UI-PLAN.md, in order. After 2.5 it looks like a phone.

- [x] 2.1 Theme scale (2x sizes). The desktop-era click tests (12 of 22) fail on the new
      positions: they get rewritten as each phone screen replaces the desktop one
- [x] 2.2 Key map: Back, Home, Recent apps, Power, volume (VM keys: Esc, Home,
      F1, F12, PgUp/PgDn)
- [ ] 2.3 Full-screen window manager, one app visible, background apps paused
- [ ] 2.4 Home screen: icon pages and All apps, no dock or top bar
- [ ] 2.5 Status bar
- [ ] 2.6 Back event in the app API, with the default behaviour in the shell
- [ ] 2.7 Touch rules: tap, long press, drag, momentum scrolling
- [ ] 2.8 On-screen keyboard
- [ ] 2.9 Quick settings panel and notifications
- [ ] 2.10 App switcher
- [ ] 2.11 Lock screen: swipe to unlock, PIN pad
- [ ] 2.12 Power menu, volume pop-up, screenshots

**Done when:** in the VM, the shell can be used with only the mouse (as a
finger) and the six stand-in keys, and each step has a test in `tests/linux/`.

## Phase 3 — Apps (in the VM)

- [ ] Settings reworked for portrait (Wi-Fi, Display, Sound, Lock screen, Home
      screen, Storage, Battery, Date and time, About phone)
- [ ] Files reworked for portrait
- [ ] Calculator reworked for portrait
- [ ] Blocksmith checked with touch
- [ ] Clock: alarms, timer, stopwatch
- [ ] Gallery: photo grid, viewer, swipe between photos
- [ ] Pick native Linux games with the brother, check they build for aarch64

**Done when:** the v1 apps work in the VM.

## Phase 4 — ARM build (QEMU)

- [ ] Buildroot config for aarch64 (Cortex-A53)
- [ ] Build the shell and apps for aarch64
- [ ] Boot in `qemu-system-aarch64` with a virtual touchscreen
- [ ] Run the test suite there, fix ARM-only bugs

**Done when:** the aarch64 image boots to the phone shell in QEMU and the
tests pass.

## Phase 5 — First boot on the phone (stock Nura)

Needs phase 0. Every flash needs the owner's OK in chat, one change per flash.

- [ ] Flash lk2nd with Heimdall, check it boots to lk2nd
- [ ] Flash the Nura Phosh boot and rootfs images with fastboot
- [ ] SSH in over USB (172.16.42.1)
- [ ] Test and write down: screen, touch, Wi-Fi, USB, storage, SD card, camera,
      brightness, vibration, battery reading, sleep/wake
- [ ] `evtest`: the real key codes for Back, Home, Recent apps, Power, volume,
      and whether the touch-key backlights work
- [ ] Check whether modem/EFS data is still intact
- [ ] Keep these images as the first known-good set

**Done when:** a written list of what works on the A5 under mainline Linux.
This decides Camera, brightness, vibration and alarms in UI-PLAN.md.

## Phase 6 — Build environment for the phone

- [ ] `pmbootstrap` in WSL2 (`.wslconfig`: memory=12GB, swap=16GB)
- [ ] Build the stock Nura samsung-a5 kernel and image ourselves
- [ ] Boot our build of it on the phone (same result as phase 5)

**Done when:** we can rebuild the phone's kernel and boot it.

## Phase 7 — Our own OS on the phone

- [ ] Our initramfs and init on the Nura kernel
- [ ] A shell (text) on the screen
- [ ] Touch input read through evdev, including multitouch (ABS_MT)
- [ ] The aarch64 LiteKern Go image from phase 4 on the phone
- [ ] Key map updated with the real codes from phase 5

**Done when:** the phone boots to the LiteKern Go shell and takes touch.

## Phase 8 — v1: hardware basics in the GUI

- [ ] Wi-Fi: scan, join, remember networks, from Settings
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
