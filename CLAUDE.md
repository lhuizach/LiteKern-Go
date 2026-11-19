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
- Where things stand (11 Nov 2026): **our system is flashed and boots from the phone's own storage** (userdata, written once with `fastboot flash userdata`, 11 Nov 2026, with the owner's yes; Nura is gone from it). Boot: lk2nd reads `extlinux/extlinux.conf` from an ext2 boot partition (label LKGO_BOOT) and loads our kernel, device tree and initramfs; our shell then uses a data disk on the same partition (settings, Wi-Fi, PIN, photos survive restarts; 2 GB FAT32). Tools in `phone/`: `make-userdata.sh` (the flashable image; flashing wipes the data), `verify-userdata.sh`, `update-system.sh` (puts new boot files on the phone over USB without touching the data: `--check` reads only, `--write extlinux|system|dtb|kernel|all`, each only with the owner's yes), and the recovery: lk2nd (power on holding Volume Down), `fastboot boot` a known good image (`Apps for zach/to-fedora/boot-litekern-goN.img`, newest go36), then run the tool from that. Apps pushed over ssh are lost at a restart; to keep a change, rebuild the system (`LKX_ARCH=aarch64 LKX_PHONE=1 bash linux/build.sh`) and `--write system`. The boot screen's static noise is lk2nd's own (it loads files into its screen memory; reserving that memory in the device tree changed nothing), so it stays. Parked: a sharper preview, faster photo saving, the untested 4160x2340 mode with JPEG, digital zoom, home screen customisation, the other games, easier app creation (tools/new-app.sh etc., and tests that find apps by name).
- Browser (13 Nov 2026): the Browser app (`apps/browser`, real JavaScript) draws pages from WPE WebKit 2.40.5, which runs in an Alpine 3.21 sandbox in the "extras" area: a 3 GiB ext4 (label LKGO_EXTRAS) in userdata at sector 4343808, after the data disk, mounted on `/extras` by `lkx-start`. Tools: `phone/make-extras.sh` and `update-system.sh --write extras` (once; refuses to replace an existing one), `phone/sandbox/` (`enter.sh` runs a command in the sandbox on the phone, `webd.c` is `lkx-webd`, the engine's host: frames in `/dev/shm/lkxweb.frame`, text commands on `/dev/shm/lkxweb.sock`; `build-webd.sh` builds it on the phone, `shot.sh` takes a screenshot, `pc-enter.sh` is the PC test sandbox in `~/lkx-web-p0`, rootless). Mobile mode by default (iPhone Safari identity string, page zoom 2); `--desktop` turns it off. The sandbox was filled over Wi-Fi from Alpine's mirror (the phone's busybox has no https; Alpine's does). JavaScript runs about 10 to 15 times slower than on the PC (JIT is on; it's the Cortex-A53), so Cloudflare's checks take a minute or more; close other apps first. Plan and numbers: `docs/BROWSER-PLAN.md`. Unbuilt: pinch zoom, tabs, a desktop-site button, the dock's Web icon still opens NetSurf. The phone is sometimes reachable only over Wi-Fi (`root@<its address>`, set `LKX_PHONE`). Apps in the background keep running; the Camera now pauses its sensor when not in front (`k86_tasks` flags).
- Sound (14 Nov 2026): works on our system. The device tree (`phone/kernel/0004-a5-sound-lpass.patch`) sends audio through the SoC's own audio block (LPASS), not the modem's DSP, because we never start the modem firmware (`remoteproc0` stays offline, which also keeps us clear of the modem partitions). One card, `samsung-a2015`: device 0 is the internal codec (earpiece, headphones; mixer routing needed, e.g. `RX1 MIX1 INP1`=RX1, `RDAC2 MUX`, `EAR_S`=Switch), device 1 is the mic capture (`CIC1 MUX`=AMIC, `DEC1 MUX`=ADC1), device 2 is the loudspeaker amplifier (TFA9895 on the quaternary port, plays with no mixer setup, and is LOUD: no hardware volume). Loudspeaker, earpiece, mic and headphones confirmed (the jack is read from the mixer control `Headphone Jack`, iface CARD; `sdk/audio.c` switches to device 0 with `HPHL`/`HPHR` and back when unplugged). Apps use `sdk/audio.h` (raw ALSA ioctls, no library; `k86_audio_open/write/close`, `k86_audio_tone`, software volume squared). The shell keeps the volume (60 at first) in `machine.txt` and tells apps through `/dev/shm/lkx-volume`; the slider pops up on the left edge. Test tools: `alsa-utils` in the sandbox (`/extras/enter.sh amixer -c 0 ...`). Also `k86_audio_beep` (non-blocking blip, a child process plays it; Brickfit uses it) and `k86_mic_open/read/close` (mono mic capture, works: Music has a Mic test button; the mic mux is `CIC1 MUX`=0 AMIC, and the first 200 ms of a recording is a pop that is dropped). Music app (`apps/music`, WAV and MP3 (minimp3 in `third_party/minimp3`, downloaded from lieff/minimp3 on 15 Nov 2026, CC0, hash in its README; MP3 length is estimated from the bit rate; not yet tried on the phone), folder `/extras/music`, `tools/push-music.sh` copies songs over ssh): lesson learned, `k86_audio_flush` does not silence the output (the hardware loops the last of the ring), only closing the stream does, so pause closes it. Every app has an icon now (`tools/make-icon-*.py`); the dock is Files, Browser, Gallery, Settings. Not built: Bluetooth audio.
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

- Camera: not working on Nura ("can't be found"), but it works on our system. Rear IMX135 driver (`phone/kernel/0002-a5-rear-camera.patch`), the Camera app (`apps/camera`): live preview 1280x720 at 30 fps, autofocus (the focus motor is DW9768-style: registers 2, 3, 4; about 0.5 to 1 s), tap to focus, and photos in a higher sensor mode (2080x1170 binned, the preview's own window, saved as 1170x2080 PNGs, about 3.5 s each; a 4160x2340 mode is in the driver but untested). The front sensor (S5K5E3YX) answers but has no driver. The phone's mode switch loses the lens power, so the app restores the lens position. Screen and Wi-Fi do work.
- Brightness: works (8 Nov 2026, go33): kernel patch `phone/kernel/0003-a5-panel-backlight.patch`, dimming by AID only (the per-level colour tables tint the screen yellow-green), changes send once when the finger lifts. Settings > Display has a Reset Screen button. A short blink on each change is the msm DSI driver's own behaviour.
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
- Home screen (15 Nov 2026): hold the home screen for edit mode (wiggle, zoom to a card, bar: Clock, Add, Icons, Pages, Done). Apps sit in any grid cell; widgets (Date, Battery, analog Clock, Music, Weather) span cells, are added from Add, moved, removed and resized by an 8-dot frame (limits in `wg_lim`); icon shape/size, app names, extra pages, and the home clock (the lock screen's clock code, own choices, date on/off) are all kept in `settings.txt` (`home-*` lines). Weather is its own app (`apps/weather`, open-meteo, city kept in `/extras/weather.cfg`); the app run as `weather --fetch` is a no-window helper the shell starts for the widget. Music keeps its state in `/dev/shm/lkx-music` and takes `/dev/shm/lkx-music-cmd` (the widget's buttons). Music's Online tab (`apps/music/online.c`, Audius, no key) streams MP3 into `/dev/shm/lkx-music-stream.mp3` through the same helper trick (`music --api` / `--dl`). The apps limit is 24 (`APPS_MAX`).
- Cover screen (`kernel/panicscreen.c`): a long hold (1 s) on the control centre's flashlight tile shows a fake memory test and kernel panic over everything; all keys and touches are ignored (no lock, no screen off, no volume pop-up) until Power is held (about 1.2 s), which "restarts" to the home screen. It is only a picture. Not tried on the phone yet.
- Folders, apps menu, Snake (16 Nov 2026): drag an icon onto another in home edit mode and hold about half a second (the one underneath lights up), then let go: a folder (`fold_*` in `kernel/desktop.c`; a folder sits on the grid as one of its apps, its face, the rest are hidden; saved in `home-layout` as `Name|g5F0` for the face and `Name|hF0` for the others; up to 8 folders of 9 apps, none in the dock, no rename yet). Tap a folder: a popup with its apps; hold an app there to take it out; the minus on a folder (edit mode) empties it. The apps menu (swipe up) is now a sheet with a Recent card (last four apps opened) and all apps A to Z on cards. Snake (`apps/snake`): swipe to steer, speeds Easy/Normal/Hard, Walls or Wrap, 4 skins, bests kept per speed and mode; it stops animating when not in front. Folders and the new apps menu are tested on the PC desktop only, not on the phone yet.
