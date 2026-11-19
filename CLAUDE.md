# LiteKern Go

LiteKern Go is a custom Linux-based phone OS for a Samsung Galaxy A5 (2015), model SM-A500FU. The phone boots Linux directly: Samsung bootloader, then lk2nd, then the Linux kernel, then our own init, then our own touch GUI. There is no Android underneath and no launcher on top. It is the phone sibling of LiteKern X and keeps its theme and feel, reworked for touch and a portrait screen.

It is built for the owner's younger brother, who wants a normal phone OS with a couple of small touches and uses the phone for games and photos sometimes. It is not meant for public release.

## Goals

1. Boot the A5 into the LiteKern Go GUI.
2. A touch-first interface in the LiteKern X style at 720x1280 portrait.
3. Reliable basics: display, touch, Wi-Fi, USB, storage.
4. The brother's extras (see Brother's extras below).

Proposed v1 is done (and has been since 7 Nov 2026): the phone boots to the GUI, takes touch input and joins Wi-Fi.

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
2. lk2nd (msm8916-mainline project, 23.1). Small second-stage bootloader that loads the kernel. Borrowed. Lives in the phone's BOOT partition.
3. Linux kernel 6.12.1-msm8916. Built from the Nura v26.06 samsung-a5 kernel config and device tree plus our six patches in `phone/kernel/`. Borrowed and tweaked.
4. Init and userland. Ours (`lkx-start`, built by Buildroot). Replaces the Nura userland and starts the GUI.
5. LiteKern Go GUI (`lkx-shell`) and apps. Ours. LiteKern X look and feel, drawn for touch.

## Safety rules (follow these strictly)

- Never run a flashing command (`heimdall flash`, `fastboot flash`, `fastboot boot`, Odin) without the owner's explicit confirmation in chat, every time.
- Only flash files built for `samsung-a5` / SM-A500FU. Files for the A5 2016 (`a5xelte`) or A5 2017 (`a5y17lte`) are different phones and must never be flashed.
- Do not touch EFS, modem or baseband partitions. The IMEI and modem calibration data are not recoverable from a firmware package.
- Change one thing per flash, so a failure points at one cause.
- Keep the last known-good boot and root filesystem images so the phone can be reflashed fast.
- The stock BTU firmware must be downloaded before the first flash. It is the way back, flashed with Odin from download mode. (It was downloaded; the way back has never been tested, see Status.)
- Download tools only from official sources and scan unsigned tools on VirusTotal before running them.
- Do not suggest or perform anything that needs admin rights unless the owner says they have the code. (From 19 Nov 2026 the owner has Windows admin on the new PC, so native Windows tools may be suggested there. Every other rule here still applies.)
- Never open, search or list `private/`, or any folder of the owner's holding IMEI, serial or MAC details. Never ask for those values or put them in chat, code or git.

## Environment

- Main PC (from 19 Nov 2026): a new Windows 11 laptop where the owner has admin. Windows user `User`, WSL2 Ubuntu user `user`. The old PC (`GamingPC`, user `lhuir`) is retired but still holds the things listed under "Not in git" until they are copied.
- Build environment: WSL2 (Ubuntu) for the kernel, Buildroot and cross-compiling. USB passthrough into WSL2 is still unreliable, so flash from Windows (native `fastboot` and `heimdall`; the Zadig WinUSB driver is installed; tools and lk2nd are in `phone-tools/`). `phone/update-system.sh` only needs ssh over the phone's USB network (172.16.42.1) and runs in WSL.
- The Fedora VM (VirtualBox) was how the first flashes were done (Heimdall for lk2nd, fastboot for userdata). It is no longer needed for flashing. It may still hold Samsung's own `boot.img` (`~/phone/stock/boot.img`, unchecked).
- If building in WSL2, set `memory=12GB` and `swap=16GB` in `.wslconfig` and keep the Linux disk on the drive with free space.
- The Android Studio emulator only runs Android images, so it is not useful here.

## Development workflow

1. PC desktop first: the shell runs on the PC (`make -f platform/Makefile run`, or headless for checks) at 720x1280 with the mouse as the touchscreen. `bash tools/check-app.sh NAME` (about a minute) and `bash tests/linux/test-linux.sh` (59 tests, about 10 minutes).
2. The phone: `bash tools/push-app.sh NAME` puts one app on the running phone over ssh (lost at a restart); `bash tools/build-system.sh` then `phone/update-system.sh --write system` (owner's yes) makes a change permanent.
3. QEMU (`qemu-system-aarch64`) was planned to catch ARM-only bugs and has not been needed.

A PC cannot test the real touch controller, Wi-Fi chip, display driver, brightness, speed on the Snapdragon 410, battery use, or the boot process on this phone.

## GUI notes

- Style: Adwaita dark by default, flat like Phosh on Nura: solid surfaces, no live blur or glass (too slow on the Cortex-A53 and it looked poor). Panels slide over a kept copy of the screen, so each frame is one copy and a panel. Taps replace clicks, drag to scroll, a portrait layout, larger touch targets.
- The GUI is the owner's own C code from LiteKern X, ported (decided and done), with the KERN86 app architecture (`.lkx` format, `kerns.json` manifest, `kern86.h` API). Apps: `docs/NEW-APP.md`, `docs/APPS.md`, `kernel/kern86_abi.h`.
- Draws to the DRM framebuffer, reads touch through evdev. Software rendering.
- An on-screen keyboard exists (`kernel/osk.c`).
- Memory is tight at 2 GB, so avoid heavy toolkits and long-running background services. Apps in the background keep running: stop animating when not in front (`k86_tasks` flags, as Camera and Snake do).

## Status

- Our system runs on the phone from RAM (`fastboot boot`, `go14`, 7 Nov 2026): GUI, touch, Wi-Fi and the R3lay app all confirmed, so proposed v1 works.
- Where things stand (11 Nov 2026): **our system is flashed and boots from the phone's own storage** (userdata, written once with `fastboot flash userdata`, 11 Nov 2026, with the owner's yes; Nura is gone from it). Boot: lk2nd reads `extlinux/extlinux.conf` from an ext2 boot partition (label LKGO_BOOT) and loads our kernel, device tree and initramfs; our shell then uses a data disk on the same partition (settings, Wi-Fi, PIN, photos survive restarts; 2 GB FAT32). Tools in `phone/`: `make-userdata.sh` (the flashable image; flashing wipes the data and the extras area), `verify-userdata.sh`, `update-system.sh` (puts new boot files on the phone over USB without touching the data: `--check` reads only, `--write extlinux|system|dtb|kernel|all`, each only with the owner's yes), and the recovery: lk2nd (power on holding Volume Down), `fastboot boot` a known good image (`Apps for zach/to-fedora/boot-litekern-goN.img`, newest go36), then run the tool from that. Apps pushed over ssh are lost at a restart; to keep a change, rebuild the system (`LKX_ARCH=aarch64 LKX_PHONE=1 bash linux/build.sh`) and `--write system`. The boot screen's static noise is lk2nd's own (it loads files into its screen memory; reserving that memory in the device tree changed nothing), so it stays. Parked: a sharper preview, faster photo saving, JPEG for the big photo mode, camera digital zoom, the other games, front-camera tuning.
- Phone layout: BOOT = lk2nd (12.5 MiB, never put Nura's boot.img there). USERDATA holds our whole disk: an MBR, p1 = LKGO_BOOT ext2 from sector 2048 (64 MiB, the only thing `update-system.sh` writes), p2 = the data disk (type 0xDA) from sector 133120, and the extras area (LKGO_EXTRAS) at sector 4343808. EFS, modem and the bootloaders are never touched; the modem firmware is never started.
- Browser (13 Nov 2026): the Browser app (`apps/browser`, real JavaScript) draws pages from WPE WebKit 2.40.5, which runs in an Alpine 3.21 sandbox in the "extras" area: a 3 GiB ext4 (label LKGO_EXTRAS) in userdata at sector 4343808, after the data disk, mounted on `/extras` by `lkx-start`. Tools: `phone/make-extras.sh` and `update-system.sh --write extras` (once; refuses to replace an existing one), `phone/sandbox/` (`enter.sh` runs a command in the sandbox on the phone, `webd.c` is `lkx-webd`, the engine's host: frames in `/dev/shm/lkxweb.frame`, text commands on `/dev/shm/lkxweb.sock`; `build-webd.sh` builds it on the phone, `shot.sh` takes a screenshot, `pc-enter.sh` is the PC test sandbox in `~/lkx-web-p0`, rootless). Mobile mode by default (iPhone Safari identity string, page zoom 2); `--desktop` turns it off. The sandbox was filled over Wi-Fi from Alpine's mirror (the phone's busybox has no https; Alpine's does). JavaScript runs about 10 to 15 times slower than on the PC (JIT is on; it's the Cortex-A53), so Cloudflare's checks take a minute or more; close other apps first. Plan and numbers: `docs/BROWSER-PLAN.md`. Unbuilt: pinch zoom, tabs, a desktop-site button, the dock's Web icon still opens NetSurf. The phone is sometimes reachable only over Wi-Fi (`root@<its address>`, set `LKX_PHONE`).
- Sound (14 Nov 2026): works on our system. The device tree (`phone/kernel/0004-a5-sound-lpass.patch`) sends audio through the SoC's own audio block (LPASS), not the modem's DSP, because we never start the modem firmware (`remoteproc0` stays offline, which also keeps us clear of the modem partitions). One card, `samsung-a2015`: device 0 is the internal codec (earpiece, headphones; mixer routing needed, e.g. `RX1 MIX1 INP1`=RX1, `RDAC2 MUX`, `EAR_S`=Switch), device 1 is the mic capture (`CIC1 MUX`=AMIC, `DEC1 MUX`=ADC1), device 2 is the loudspeaker amplifier (TFA9895 on the quaternary port, plays with no mixer setup, and is LOUD: no hardware volume). Loudspeaker, earpiece, mic and headphones confirmed (the jack is read from the mixer control `Headphone Jack`, iface CARD; `sdk/audio.c` switches to device 0 with `HPHL`/`HPHR` and back when unplugged). Apps use `sdk/audio.h` (raw ALSA ioctls, no library; `k86_audio_open/write/close`, `k86_audio_tone`, software volume squared). The shell keeps the volume (60 at first) in `machine.txt` and tells apps through `/dev/shm/lkx-volume`; the slider pops up on the left edge. Test tools: `alsa-utils` in the sandbox (`/extras/enter.sh amixer -c 0 ...`). Also `k86_audio_beep` (non-blocking blip, a child process plays it; Brickfit and Snake use it) and `k86_mic_open/read/close` (mono mic capture, works: Music has a Mic test button; the mic mux is `CIC1 MUX`=0 AMIC, and the first 200 ms of a recording is a pop that is dropped). Music app (`apps/music`, WAV and MP3 (minimp3 in `third_party/minimp3`, downloaded from lieff/minimp3 on 15 Nov 2026, CC0, hash in its README; MP3 length is estimated from the bit rate; not yet tried on the phone), folder `/extras/music`, `tools/push-music.sh` copies songs over ssh): lesson learned, `k86_audio_flush` does not silence the output (the hardware loops the last of the ring), only closing the stream does, so pause closes it. Every app has an icon (`tools/make-icon-*.py`); the dock is Files, Browser, Gallery, Settings. Not built: Bluetooth audio.
- Camera (16 Nov 2026): rear and front both work on our system, see Known risks. Gallery: pinch zoom with two fingers (`platform/touch.c` publishes the second finger in `/dev/shm/lkx-touch2`, apps read it with `sdk/touch2.h`) and a loading spinner while a picture opens.
- Stock firmware: `A500FUXXU1CSB1` BTU, the exact build on the phone, in `Apps for zach/to-fedora/SAMFW.COM_SM-A500FU_BTU_A500FUXXU1CSB1_fac.zip` (not in git). From samfw.com, a third party (Frija found nothing: Samsung no longer serves this model), so not proven to be Samsung's. Zip MD5 matches the page, all four parts' checksums pass, contents are firmware images only. A restore should leave out the CP (modem) file: see the safety rules.
- Way back to Android unproven and left that way: the owner decided on 7 Nov 2026 to skip testing it, accepting the risk. `phone/restore-stock.sh` (Heimdall, Linux) has never been run; on the new PC Odin or native Windows Heimdall is the likely route. KNOX is probably tripped (never read). BOOT holds lk2nd and USERDATA is ours, so Android does not boot until Samsung's `boot.img` is flashed back.
- Folders, apps menu, Snake (16 Nov 2026): drag an icon onto another in home edit mode and hold about half a second (the one underneath lights up), then let go: a folder (`fold_*` in `kernel/desktop.c`; a folder sits on the grid as one of its apps, its face, the rest are hidden; saved in `home-layout` as `Name|g5F0` for the face and `Name|hF0` for the others; up to 8 folders of 9 apps, none in the dock, no rename yet). Tap a folder: a popup with its apps; hold an app there to take it out; the minus on a folder (edit mode) empties it. The apps menu (swipe up) is now a sheet with a Recent card (last four apps opened) and all apps A to Z on cards. Snake (`apps/snake`): swipe to steer, speeds Easy/Normal/Hard, Walls or Wrap, 4 skins, bests kept per speed and mode; it stops animating when not in front. Snake was tried on the phone (pushed over ssh, owner said it was fine). Folders also work on the phone (owner confirmed 19 Nov 2026, no issues), and so does Snake. The full test suite was not rerun after folders and the new apps menu.
- Git (19 Nov 2026): LiteKern-Go `main` on GitHub is current (`d87c960`). LiteKern X: GitHub `main` is the old custom-kernel line with the owner's README; the Linux-based LiteKern X is the `linux-version` branch of `lhuizach/LiteKern-X` (it used to exist only in `LiteKern-X-Linux` on the old PC).

## Not in git (copy to a new PC; the old PC's `to-new-pc` folder has these, with a `HANDOFF.yaml` and `MANIFEST.tsv`)

- `~/.ssh/litekern-phone` (ed25519, root ssh into the phone; secret, `chmod 600`) and `phone/authorized_keys` (its public half, baked into the system image).
- `~/lkx-phone/stock-wlan/out/wcnss.*` (Wi-Fi firmware, out of the stock `NON-HLOS.bin`), `~/lkx-phone/firmware/` (copied from a running Nura), `~/lkx-phone/nura/boot.img` (template for `make-ramboot.sh`).
- `apps/r3lay/r3lay.conf` (project URL and public key), the recovery images in `Apps for zach/to-fedora/` (newest `boot-litekern-go36.img`, `lk2nd-msm8916.img`), the stock firmware zip, and `private/` (never opened).
- The system build fails in `phone/make-rootfs-overlay.sh` without the `wcnss.*` files.

## Roadmap

1. Prep (done): drivers, tools, stock firmware downloaded, `heimdall detect` sees the phone.
2. First boot (done): lk2nd and Nura Phosh flashed, ssh over USB, screen, touch, Wi-Fi tested.
3. Build environment (done): WSL2, our own kernel build.
4. Own minimal OS (done): our init and a shell on screen, touch working.
5. GUI (done): DRM drawing, evdev touch, the LiteKern X GUI ported.
6. Polish and the brother's extras (now): games, apps, camera and home screen polish.

## Known risks

- Camera: not working on Nura ("can't be found"), but it works on our system. Rear IMX135 driver (`phone/kernel/0002-a5-rear-camera.patch`), the Camera app (`apps/camera`): live preview 1280x720 at 30 fps, autofocus (the focus motor is DW9768-style: registers 2, 3, 4; about 0.5 to 1 s), tap to focus, and photos in two modes: 2080x1170 (binned, saved as 1170x2080 PNGs, about 3.5 s each) and the full 4160x2340 (about 7 s from the shutter, an 18.7 MB PNG). The front sensor (S5K5E3YX, driver `phone/kernel/0005-a5-front-camera.patch` plus `0006-camss-csid1-uses-vfe0.patch`, notes in `docs/FRONT-CAMERA.md`) works: a front/back button, colours and orientation fixed, photos match the preview. It has no autofocus and no lens shading or white balance tuning yet. The phone's mode switch loses the lens power, so the app restores the lens position. Screen and Wi-Fi do work.
- Brightness: works (8 Nov 2026, go33): kernel patch `phone/kernel/0003-a5-panel-backlight.patch`, dimming by AID only (the per-level colour tables tint the screen yellow-green), changes send once when the finger lifts. Settings > Display has a Reset Screen button. A short blink on each change is the msm DSI driver's own behaviour.
- Games: Android games will not install on Linux. Waydroid might run them but is heavy on 2 GB RAM and unverified on this kernel.
- Calls and mobile data are the hardest parts on mainline Linux and are not planned early.
- Other reported gaps: USB-OTG.
- Flashing userdata wipes the user's data and the extras area; flashing wiped Android's userdata long ago.
- Some guides and links still use the old postmarketOS name.

## Brother's extras

His list, from the owner, 7 Nov 2026. Games are our own originals with our own names and art, not copies of the named games.

- Games: a lane-dodging traffic racer (not built), Snake (done, `apps/snake`), a block-placing puzzle like Block Blast (done: Brickfit), a falling-block puzzle like Tetris (done 19 Nov 2026: Stackfall, `apps/stackfall`, drag/tap/flick controls, hold, ghost, next 3, saved best; checked on the PC desktop only, not on the phone, and line clears were not seen on screen yet), a knife-throwing game with a watermelon ("Watermelon tungin": not built, exact idea to confirm).
- Apps: R3lay (the owner's own messaging app: a Capacitor web app on Supabase, with realtime chat, file storage and WebRTC calls; our own C client for text chat is in `apps/r3lay/`, see `docs/R3LAY-PLAN.md`; calls unlikely), Class Charts (no Android apps here, so it would be our own client, if its login and data can be reached at all: unverified), Weather (done), a music app such as Spotify (Spotify itself isn't available for this OS: the local Music app with an Audius online tab is done).
- All the online apps need Wi-Fi, which works.

## Open decisions

- Confirm with the brother that no APK games are acceptable (the camera question is settled: it works).
- Whether to build the missing games next (racer, falling-block puzzle, watermelon knife game) or Class Charts, a USB drive mode, or more camera work.

## Working style

- Keep replies short and direct, casual tone.
- Keep configs and scripts under git and commit as you go.
- Say what is unverified instead of guessing, especially for hardware and flashing steps.
- Home screen (15 Nov 2026): hold the home screen for edit mode (wiggle, zoom to a card, bar: Clock, Add, Icons, Pages, Done). Apps sit in any grid cell; widgets (Date, Battery, analog Clock, Music, Weather) span cells, are added from Add, moved, removed and resized by an 8-dot frame (limits in `wg_lim`); icon shape/size, app names, extra pages, and the home clock (the lock screen's clock code, own choices, date on/off) are all kept in `settings.txt` (`home-*` lines). Weather is its own app (`apps/weather`, open-meteo, city kept in `/extras/weather.cfg`); the app run as `weather --fetch` is a no-window helper the shell starts for the widget. Music keeps its state in `/dev/shm/lkx-music` and takes `/dev/shm/lkx-music-cmd` (the widget's buttons). Music's Online tab (`apps/music/online.c`, Audius, no key) streams MP3 into `/dev/shm/lkx-music-stream.mp3` through the same helper trick (`music --api` / `--dl`). The apps limit is 24 (`APPS_MAX`).
- Cover screen (`kernel/panicscreen.c`): a long hold (1 s) on the control centre's flashlight tile shows a fake memory test and kernel panic over everything; all keys and touches are ignored (no lock, no screen off, no volume pop-up) until Power is held (about 1.2 s), which "restarts" to the home screen. It is only a picture. Not tried on the phone yet.
