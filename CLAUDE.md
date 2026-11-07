# LiteKern Go

LiteKern Go is a custom Linux-based phone OS for a Samsung Galaxy A5 (2015), model SM-A500FU. The phone boots Linux directly: Samsung bootloader, then lk2nd, then the Linux kernel, then our own init, then our own touch GUI. There is no Android underneath and no launcher on top. It is the phone sibling of LiteKern X and keeps its theme and feel, reworked for touch and a portrait screen.

It is built for the owner's younger brother, who wants a normal phone OS with a couple of small touches and uses the phone for games and photos sometimes. It is not meant for public release.

## Goals

1. Boot the A5 into the LiteKern Go GUI.
2. A touch-first interface in the LiteKern X style at 720x1280 portrait.
3. Reliable basics: display, touch, Wi-Fi, USB, storage.
4. The brother's extras (see Brother's extras below).

Proposed v1 is done when the phone boots to the GUI, takes touch input and joins Wi-Fi.

Accepted trade-offs: some parts will be buggy, and Android apps (APKs) will not run.

## Target device

- Model: SM-A500FU, region code (CSC) BTU (UK)
- Chip: Qualcomm Snapdragon 410 (MSM8916), 64-bit ARM Cortex-A53, Adreno 306 GPU. Build for aarch64.
- RAM 2 GB, storage 16 GB plus microSD, screen 5 inch 720x1280
- Stock OS: Android 6.0.1, kernel 3.10.49, baseband A500FUXXU1CPI2
- Bootloader: Samsung official, never modified. Download mode works (Vol Down + Home + Power, then Vol Up).
- Names: `a5ulte` on Android, `samsung-a5` in Nura (formerly postmarketOS, renamed Nura on 27 Sept 2026, site nura.eco, images at images.nura.eco)

## Architecture (boot order, bottom to top)

1. Samsung bootloader. Left untouched. Provides download mode and loads lk2nd from the boot partition.
2. lk2nd (msm8916-mainline project). Small second-stage bootloader that loads the kernel. Borrowed.
3. Linux kernel. Starts from the Nura samsung-a5 kernel config and device tree (mainline msm8916). Borrowed, may be tweaked later.
4. Init and userland. Ours. Replaces the Nura userland and starts the GUI.
5. LiteKern Go GUI and apps. Ours. LiteKern X look and feel, drawn for touch.

## Safety rules (follow these strictly)

- Never run a flashing command (`heimdall flash`, `fastboot flash`, `fastboot boot`, Odin) without the owner's explicit confirmation in chat, every time.
- Only flash files built for `samsung-a5` / SM-A500FU. Files for the A5 2016 (`a5xelte`) or A5 2017 (`a5y17lte`) are different phones and must never be flashed.
- Do not touch EFS, modem or baseband partitions. The IMEI and modem calibration data are not recoverable from a firmware package.
- Change one thing per flash, so a failure points at one cause.
- Keep the last known-good boot and root filesystem images so the phone can be reflashed fast.
- The stock BTU firmware must be downloaded before the first flash. It is the way back, flashed with Odin from download mode.
- Download tools only from official sources and scan unsigned tools on VirusTotal before running them.
- Do not suggest or perform anything that needs admin rights unless the owner says they have the code.
- Never open, search or list `private/`, or any folder of the owner's holding IMEI, serial or MAC details. Never ask for those values or put them in chat, code or git.

## Environment

- Main PC: Windows 11, 750 GB free disk, 16 GB DDR4 RAM. The current account has no admin rights yet (the owner can get the code later).
- Build environment: WSL2 (Ubuntu) for the kernel, `pmbootstrap` and cross-compiling. USB passthrough into WSL2 is unreliable. The Fedora 44 VirtualBox VM has Heimdall and fastboot and `heimdall detect` works there over USB passthrough (detect only: a real flash through it is untested).
- If building in WSL2, set `memory=12GB` and `swap=16GB` in `.wslconfig` and keep the Linux disk on the drive with free space.
- VirtualBox is installed. The Android Studio emulator only runs Android images, so it is not useful here.

## Development workflow

1. VirtualBox first: build an x86_64 version and run it in a VM with a 720x1280 portrait screen, using the mouse as the touchscreen.
2. QEMU second: `qemu-system-aarch64` with a virtual touchscreen to catch ARM-only bugs.
3. The phone last: flash lk2nd with Heimdall, then the kernel and root filesystem with fastboot. Boot the stock Nura Phosh image once first to see what works.

A VM cannot test the real touch controller, Wi-Fi chip, display driver, brightness, speed on the Snapdragon 410, battery use, or the boot process on this phone.

## GUI notes

- Style: Adwaita dark by default, flat like Phosh on Nura: solid surfaces, no live blur or glass (too slow on the Cortex-A53 and it looked poor). Panels slide over a kept copy of the screen, so each frame is one copy and a panel. Taps replace clicks, drag to scroll, a portrait layout, larger touch targets.
- The existing LiteKern X GUI is the owner's own code, mostly C, with the KERN86 app architecture (`.lkx` format, `kerns.json` manifest, `kern86.h` API). Check how much of it assumes x86 or X11 before porting.
- Draw to the framebuffer or DRM/KMS, read touch through evdev (or libinput). Software rendering is fine at first.
- An on-screen keyboard will be needed.
- Memory is tight at 2 GB, so avoid heavy toolkits and long-running background services.
- GUI language is undecided: port the existing C GUI, or rewrite (Rust was suggested). Ask the owner before starting.

## Status

- Our system runs on the phone from RAM (`fastboot boot`, `go14`, 7 Nov 2026): GUI, touch, Wi-Fi and the R3lay app all confirmed, so proposed v1 works, though only from RAM: a restart goes back to Nura and forgets Wi-Fi and sign-ins.
- Phone: lk2nd flashed to BOOT (Heimdall), then Nura Phosh v26.06 to userdata (fastboot), and Nura boots. Both from the Fedora VM. Android does not boot until Samsung's boot.img (`~/phone/stock/boot.img` in Fedora) is flashed back. KNOX probably tripped. Nothing else flashed. Reactivation lock was off; no accounts.
- Downloaded and scanned: Heimdall 1.4.0 (win32), Android platform-tools, `lk2nd-msm8916.img` (418 KB), Nura v26.06 samsung-a5 Phosh images (boot and rootfs, extracted), Frija 2.0.26142.10.
- Downloaded but waiting on admin to install: Zadig 2.9 (USB driver), Visual C++ 2012 x86 (Heimdall), .NET 8 Desktop Runtime x86 (Frija).
- `heimdall detect` works from the Fedora VM. Our build of the Nura kernel is ready (`phone/`), not flashed.
- Stock firmware: `A500FUXXU1CSB1` BTU, the exact build on the phone, in `DownloadsSAMFW.COM_SM-A500FU_BTU_A500FUXXU1CSB1_fac.zip`. From samfw.com, a third party (Frija found nothing: Samsung no longer serves this model), so not proven to be Samsung's. Zip MD5 matches the page, all four parts' checksums pass, contents are firmware images only. A restore should leave out the CP (modem) file: see the safety rules.
- Way back unproven and left that way: the owner decided on 7 Nov 2026 to skip testing it, accepting the risk. Odin needs Samsung's USB driver (admin?); Heimdall restore from Fedora is untested.

## Roadmap

1. Prep: install Visual C++, .NET and the Zadig driver (needs admin), run Frija for the stock firmware, confirm `heimdall detect` sees the phone.
2. First boot: flash lk2nd and the Nura Phosh image, SSH in over USB (172.16.42.1), test screen, touch, Wi-Fi, USB, storage and camera.
3. Build environment: WSL2 and `pmbootstrap`, build the stock Nura kernel and image, and boot it.
4. Own minimal OS: custom initramfs and init, a shell on screen, touch input working.
5. GUI: framebuffer or DRM drawing, touch via evdev, port the LiteKern X GUI.
6. Polish and the brother's extras.

## Known risks

- Camera: not working on Nura ("can't be found"), and no driver in our kernel yet; the brother wants to take photos sometimes. Both sensors now answer on our system (rear IMX135, front S5K5E3YX), but there is no picture yet. Screen and Wi-Fi do work.
- Games: Android games will not install on Linux. Waydroid might run them but is heavy on 2 GB RAM and unverified on this kernel.
- Calls and mobile data are the hardest parts on mainline Linux and are not planned early.
- Other reported gaps: display brightness and USB-OTG.
- Flashing wipes Android. It is unverified whether lk2nd and Nura leave modem data alone.
- Some guides and links still use the old postmarketOS name.

## Brother's extras

His list, from the owner, 7 Nov 2026. Games are our own originals with our own names and art, not copies of the named games.

- Games: a lane-dodging traffic racer, Snake, a block-placing puzzle (like Block Blast), a falling-block puzzle (like Tetris), a knife-throwing game with a watermelon ("Watermelon tungin": exact idea to confirm).
- Apps: R3lay (the owner's own messaging app: a Capacitor web app on Supabase, with realtime chat, file storage and WebRTC calls; our own C client for text chat is in `apps/r3lay/`, see `docs/R3LAY-PLAN.md`; calls unlikely), Class Charts (no Android apps here, so it would be our own client, if its login and data can be reached at all: unverified), Weather, a music app such as Spotify (Spotify itself isn't available for this OS, so likely a local player first).
- All the online apps need Wi-Fi working in our system first, and music needs sound.

## Open decisions

- Confirm with the brother that no APK games and a possibly missing camera are acceptable.
- GUI path: port the existing C GUI or rewrite it.
- Get the admin code and finish the installs.
- Download the stock firmware before any flashing.
- Boot Nura once and check whether the camera works.

## Working style

- Keep replies short and direct, casual tone.
- Keep configs and scripts under git and commit as you go.
- Say what is unverified instead of guessing, especially for hardware and flashing steps.
