# Agent prompt: remake LiteKern Go's GUI in HTML/CSS/JS

Paste everything below the line into a coding agent that runs in this repo.

---

You're working on **LiteKern Go**, my own Linux phone OS for a Samsung Galaxy A5 (2015), SM-A500FU, codename `samsung-a5`. It's for my younger brother: a normal phone for games and the odd photo, not for public release.

## The task

Remake the **GUI layer** of LiteKern Go in **HTML, CSS and JavaScript**, running fullscreen in **WPE WebKit (Cog)** directly on DRM/KMS. There's no X11, no Wayland desktop, and no Electron or Chromium. Keep the LiteKern X look and feel, reworked for touch at 720x1280 portrait.

Leave these layers alone: the Samsung bootloader, lk2nd, the Nura-based kernel in `phone/`, and the boot process. A few extra seconds of boot for WebKit is fine.

## Read these first, in this order

1. `CLAUDE.md`: the project, the device and the **safety rules**. Those rules override anything in this prompt.
2. `UI-PLAN.md`: how the phone looks and behaves (hardware keys, sizes, screens). This is the spec for the new GUI.
3. `ROADMAP.md`, especially the "WebKit (in progress)" section and `linux/buildroot-web.fragment`. WPE is already partly wired in.
4. `docs/DEVELOPING.md`, `docs/APPS.md`, `docs/NON-GOALS.md`.
5. The current C code: `platform/` (Linux backend: DRM, input, net, devices, system), `kernel/` (shell, widgets, theme, lock screen, OSK, settings), `sdk/kern86.h` and `apps/`. Work out what is GUI, which you replace, and what is backend, which you keep.

Never open, list or search `private/` (see CLAUDE.md).

## Architecture to build

```
WPE WebKit / Cog (fullscreen, DRM/KMS)
   └── shell: HTML/CSS/JS  (status bar, home, lock screen, recents, OSK, apps)
          ⇅ WebSocket on 127.0.0.1 (JSON messages), or WebKit JS bindings
backend daemon (native)
   └── Wi-Fi, battery, brightness, power/sleep, hardware keys, storage, app launch, settings
```

- **Backend:** reuse the existing C code in `platform/` where it already does the job. Write any **new** backend code in **Rust** unless there's a strong reason not to, especially the parts that parse messages from the GUI or talk D-Bus (iwd/NetworkManager, UPower). Use separate processes or FFI, and don't rewrite working C just for the sake of it. Build for both x86_64 (VM) and aarch64 (phone). For Rust, use `aarch64-unknown-linux-musl` if it works with Buildroot.
- **Protocol:** write it down in `docs/WEB-PROTOCOL.md` before coding: message names, fields and errors. Validate every incoming message in the backend, because it runs with high privileges. Bind to localhost only.
- **Frontend:** plain HTML/CSS/JS, or a very small framework if there's a clear reason. No heavy frameworks: RAM is 2 GB. No CDN or network loads; everything ships in the image. One `--scale` CSS variable, as in UI-PLAN.md.
- **Theme:** port LiteKern X's theme (Adwaita-based dark/light, accent colours, wallpapers, icons in `assets/`, springy animations) into CSS variables.
- **Touch:** tap, drag-to-scroll with momentum, long press, swipe. Touch targets of at least 88 px. Hardware keys (Back, Home, Recent, Power, Volume) come from the backend as events, not from the DOM.
- **On-screen keyboard:** an HTML component, modelled on `kernel/osk.c`.
- **Apps:** each app is a folder with HTML/JS plus a manifest, kept close to the existing `kerns.json` format. Back first goes to the app, which reports whether it handled it (see UI-PLAN.md). Decide how existing C `.lkx` apps fit in: port them, wrap them, or drop them. **Ask me** before dropping any.
- **Memory:** one app on screen at a time, background apps paused. Measure WPE's RAM use in the VM and report it.

## Order of work (stop and report at each checkpoint)

1. **Plan.** After reading, write `docs/WEB-REMAKE-PLAN.md`: what you keep, what you replace, the file layout, the protocol outline and open questions. **Stop and wait for my OK.**
2. **Desktop browser prototype.** The shell runs in a normal browser on my Windows PC, with a fake backend (mock WebSocket) and the mouse as touch, at 720x1280. Status bar, home screen, lock screen, one app (Calculator) and the OSK. **Checkpoint.**
3. **Real backend.** The daemon serves the shell over WebSocket in the x86_64 VM (`vm/`, `linux/build.sh`, `LKX_WEB=1`). It covers Wi-Fi scan/join (if the VM can test it), settings saved to disk, and app launch. Init starts the daemon and then Cog fullscreen. **Checkpoint**, with RAM use and boot time.
4. **Port the apps:** Settings, Files, Gallery, Clock, Task Manager, Recent apps, then Blocksmith. One commit per app.
5. **aarch64 in QEMU** (`vm/qemu-arm.sh`) to catch ARM-only bugs. **Checkpoint.**
6. **Phone:** don't flash anything. Prepare the images and a written test plan, then hand over to me. Check whether `freedreno` (Adreno 306) gives WPE GPU acceleration. That's unverified, so say so, and plan for a softpipe/software fallback.

## Hard rules

- **Never** run `heimdall flash`, `fastboot flash`, `fastboot boot` or Odin, or anything else that writes to the phone. I do every flash myself, after confirming in chat.
- Never touch EFS, modem or baseband partitions, and only use `samsung-a5` files (not `a5xelte` or `a5y17lte`).
- Nothing that needs admin rights on my Windows PC unless I say I have the code.
- Download tools only from official sources.
- Don't put IMEI, serial or MAC values anywhere.
- Commit as you go, with small clear commits. Keep configs and scripts in git.
- Say what's **unverified** instead of guessing, especially for hardware: GPU, touch controller, key codes, Wi-Fi chip, camera.
- Ask me before big or irreversible choices: deleting the C GUI code, changing the app format, adding a framework, or anything that changes the boot chain.
- Keep the old C GUI working until the web GUI matches it in the VM, so there's always a fallback.

## Known risks to keep in mind

- **GPU:** WPE without GPU acceleration may be laggy at 720x1280 on the Snapdragon 410.
- **Camera:** it may not work under Linux on this phone at all. If it does, the route is GStreamer and libcamera. Don't build a camera app until it's been checked on the phone.
- **Battery:** keep the shell idle when nothing changes. No timers or animations running offscreen.
- **RAM:** 2 GB in total.

## Style

Short, direct, casual replies. At each checkpoint, give me what works, what doesn't, what's unverified and what you need from me.
