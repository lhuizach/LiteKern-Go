# LiteKern X — Phase 5: Expansion
**Budget: not set yet. Estimate each section before starting it.**
**Prerequisite: Phase 4 fully done (see 04-LiteKernX-Publishing.md Done Criteria). v1.0 is released first.**

Goal: the things removed from `docs/NON-GOALS.md` on 2026-09-29 that are too big for v1.0. Each section is its own release (v1.1, v1.2, …). The One Rule still applies: **one section at a time, each finished before the next starts.** The order below is a suggestion, most useful on a laptop first; reorder deliberately, not by drift.

---

## 1. ACPI power management
- [ ] Clean shutdown and reboot through ACPI (not just "hold the power button") — **shutdown done early (2026-10-02, the user's request)**: `kernel/acpi.c` finds RSDP -> RSDT -> FADT -> DSDT and reads `_S5` with a minimal AML reader (one package of numbers), then writes SLP_TYP | SLP_EN to PM1a/PM1b; works in QEMU (tested) and VirtualBox. Still to do here: check it on the EeePC, and reboot through the FADT reset register (restart still uses the keyboard controller)
- [ ] Battery status: charge level and charging state, shown in the shell
- [ ] Backlight brightness and the Fn keys that control it
- [ ] Suspend to RAM (resume must restore the display mode and PS/2 devices)
- [ ] Decide first: a minimal hand-written AML reader for the EeePC's tables, or a ported interpreter (ACPICA)? This decides the size of the whole section — for shutdown the minimal reader was enough; battery and suspend need methods (_BST, _PTS), so the question stays open for them

## 2. Audio
- [ ] Intel HD Audio controller driver, through the driver layer
- [ ] Play a PCM buffer (enough for UI sounds and one app)
- [ ] Volume control, including the Fn keys

## 3. USB
- [ ] UHCI/EHCI host controller driver
- [ ] USB mass storage, so the OS can read and write the stick it booted from
- [x] Persistent settings: save the Phase 3 §5 theme choices — **done early (2026-10-09, the user's request), without a USB driver:** the stick is written through the BIOS (FAT32, Phase 2 §5a). `/LiteKern/settings.txt` and a packed custom wallpaper (`kernel/settings.h`, `kernel/deflate.c`)
- [ ] USB keyboard/mouse (HID) only if a real need turns up; the built-in ones are PS/2

## 4. Networking
- [ ] Atheros Ethernet driver (`1969:1026`, found on bus 3 in Phase 1 §3)
- [ ] Network stack: IPv4, ARP, DHCP, UDP, then TCP
- [ ] Socket syscalls for KERN86 apps, added only as an app needs them
- [ ] Wi-Fi (`168c:002a`) last, if at all: it needs firmware, 802.11 and WPA, and is by far the biggest item in this phase

## 5. Multiple users and the lock screen (planned 2026-10-09, the user's request; brought forward)
Needed §3's writable storage, which came early: settings already save to the boot stick (`kernel/settings.h`, 2026-10-09). Four stages, each finished and tested before the next. Decided with the user:
- **Passwords are optional**, per user. Without one, the lock screen unlocks with a click or a key
- **Login at start only when it's needed:** when there's more than one user, or the one user has a password. One user without a password goes straight to the desktop, as now
- **Users don't see each other's files, unless they're an admin.** The first user (made at first start) is the admin; admins see every user's folder in Files and add or remove users. FAT32 has no permissions, so this is Files hiding the folders, not real protection: said plainly in the docs and in Settings
- **Honest limit:** the stick isn't encrypted. A password stops someone using the EeePC, not someone reading the stick in another computer
- **No fast user switching** at first: two people's apps open at once doesn't fit the EeePC's memory and the 8-window limit. Log Out closes your apps

**Stage 1: the lock screen** (ships alone first, and gets used on the EeePC before Stage 2 starts)
- [ ] In the kernel's shell, like the screen saver, so no app can get round it: apps keep running behind it but get no input, and their windows are never drawn on it
- [ ] Big clock: the time in large digits (a new clock face, digits and colon only, about 96 px light, rasterised at build time like the UI font), the date under it, updated each minute; 12/24-hour; seconds optional
- [ ] Its wallpaper, dimmed so the clock reads: the desktop's, any preset, or a picture of your own (Files' Set Wallpaper gains "for the lock screen")
- [ ] The power menu stays on it: Shut Down, Restart
- [ ] Locking: Lock in the power menu, Super+L, and optionally when the screen saver ends. Unlocking (no users yet): a click or a key
- [ ] Saved with the other settings; QEMU tests: lock, unlock, the clock rolling over, input not reaching apps

**Stage 2: users and passwords**
- [ ] First start: a setup screen (name, optional password, avatar: a picture or a letter tile). That user is the admin
- [ ] The login screen: the users to pick from, the password field, the big clock and the power menu
- [ ] Passwords never stored as typed: salted and hashed (SHA-256, many rounds) in `/LiteKern/users.txt`. A wrong one shakes the field and waits a moment before the next try; a Caps Lock warning
- [ ] System calls for users (list, check a password, add, change), checked by the kernel: changing an account needs its password, adding or removing users needs an admin's, so a misbehaving app can't take one over
- [ ] Tests: setup, a wrong password then the right one, the delay

**Stage 3: per-user everything**
- [ ] Each user's own style, accent, desktop and lock wallpapers, screen saver, Blocksmith projects: `/LiteKern/users/<name>/`. Today's settings file becomes the first user's
- [ ] Home folders, `/Users/<name>/` (Documents, Pictures); Files opens there and hides other users' folders, unless you're an admin
- [ ] Log Out (power menu): closes your apps, back to the login screen
- [ ] Tests: a second user's settings and folders kept apart; an admin sees both

**Stage 4: Settings pages**
- [ ] Users: add (admin), rename, change the password (asks for the old one), change the avatar, remove (admin; asks what to do with their files)
- [ ] Lock screen: its wallpaper, the clock format, lock when the screen saver ends
- [ ] Power: the screen saver time, lock after N minutes idle. Lid close (lock or nothing) once the lid's state can be read (the probe finds the lid, `PNP0C0D`, but doesn't read it yet), and the battery on the lock screen once §1's battery work lands: until then those rows say "not available yet"

- [ ] File permissions, if a filesystem that has them exists by then

## 6. Localisation
- [ ] Keyboard layouts other than US (still a non-goal in `docs/NON-GOALS.md` until this section starts)
- [ ] Translatable UI strings; non-Latin text needs a font that covers it

## 7. x86-64 (long mode)
- [ ] **The EeePC's Atom N280 is 32-bit only and can't run 64-bit code.** This only makes sense together with new target hardware, and "hardware other than the EeePC" is still a non-goal. Decide the hardware first, then move it off `docs/NON-GOALS.md`
- [ ] 64-bit paging, GDT/IDT, syscall entry, and a 64-bit KERN86 ABI

## 8. Extensions (planned 2026-10-02, the user's request)
Customise the UI with code you can edit inside LiteKern X, AwesomeWM-style: **extensions written in Lua**, run by the OS, reloaded without a rebuild or a reboot. It needs nothing from the other sections, so it can be the first one done in this phase. Decided with the user:
- **Language: Lua** (small, embeddable, made for this), not C compiled in the OS (that would mean porting a compiler and risking unbootable kernels) nor JavaScript (a much bigger engine)
- **Scope of v1: add and tweak**: extensions add to and adjust the built-in UI. Rewriting the top bar and dock themselves in Lua ("the whole shell is code you can edit") is a possible later step, once the API has proven itself
- **Preinstalled, no online store**: the official extensions ship on the stick, switched off; an online catalog would need §4 first and isn't planned
- **One app to find and edit them**, plus a general Text Editor for any text file

Foundations (apps can't do these yet):
- [ ] Writing files: FAT32 file writes and a SYS_FS_WRITE call (apps can create, rename, delete and read, but not write contents)
- [ ] Bigger apps: memory that grows (an sbrk-style call, `malloc` in the SDK) beyond today's fixed 1 MiB image, and a small C library in the SDK (strings, formatted output, file I/O over the calls)
- [ ] Floating point for apps (Lua needs it): the kernel saves and restores each app's FPU state (FXSAVE) when it switches apps, which it only does at system calls. The kernel itself stays integer-only
- [ ] Lua 5.4 ported as an SDK library (MIT licence, noted next to it like the font's)

The pieces:
- [ ] **Text Editor** app: open any text file from Files, type, scroll, select, undo, find, Ctrl+S
- [ ] **Extension host**: one ring 3 process runs every enabled extension, isolated like any app. An extension that errors is switched off and the Log app shows the file and line; if the host crashes or hangs (the watchdog), the desktop carries on with the built-in UI. **Safe mode**: holding Shift at boot loads no extensions
- [ ] **Shell API v1** for Lua: top bar widgets (add, hide the built-in ones), the dock (pins, order, click behaviour, size), desktop widgets, keyboard shortcuts, window rules (placement, size, snapping), colours and shapes, animation speed and bounce, and events (app opened/closed, focus changed, a minute passed, boot finished). Extensions draw with the same gfx/text/widget calls the apps use. Documented in `docs/EXTENSIONS.md`
- [ ] An extension is a folder, `/Extensions/<name>/`, with `extension.json` (name, version, description, entry, what it uses) and its Lua files
- [ ] **Extensions app**:
  - a list of every extension with on/off switches (applied at once), descriptions, what each one uses, and its last error
  - **+ New** from working templates
  - **Edit** opens the code in the Text Editor's view, with line numbers, Lua colouring, the error line highlighted, and **Save & Reload**
  - preinstalled ones can be edited freely, with **Reset to original**
- [ ] **Starter extensions**, preinstalled and off: Seconds Clock, Window Snapper (keyboard snapping), System Monitor (memory, apps open), Sticky Notes, Dock Tweaks, Theme Switcher. They double as examples
- [ ] Tests: the API from a test extension; a broken extension (a syntax error, a runtime error, an endless loop) never takes the desktop down; safe mode

## 9. Monitor and fan control (planned 2026-10-03, the user's request; brought forward)
**A deliberate exception to the phase order**, like §1's shutdown: started before v1.0 on the user's decision. A lightweight system monitor with fan control, built for the EeePC. Design, safety rules and stages: `docs/MONITOR.md`. Decided with the user:
- **The kernel is the privileged helper.** Ring 3 can't reach ports or MSRs, so hardware access, the fan's safety floors, the emergency override and the hand-back to firmware live in the kernel, behind KERN86 calls. A crashed or stopped app can't leave the fan in manual mode
- **The EC is read directly**, through a register map worked out from the EeePC's ACPI tables, not by running AML (no interpreter, no ACPICA). §1's AML question stays open for battery methods and suspend
- **RAM only for now:** no FAT32 file writes yet, so settings, profiles and history last until a restart; CSV export and profile import/export wait for §8's SYS_FS_WRITE
- **Monitoring only** until the register map is verified for the BIOS version and the user confirms a warning; that confirmation is asked again after every restart

Stages, each finished before the next:
- [x] **(a) Hardware probe** (2026-10-03): `kernel/hwprobe.c` and the Hardware Probe app list what this machine has and what LiteKern X can reach. Read-only: `rdmsr_safe` (a missing MSR can't panic), the CPU's thermal sensor and SpeedStep MSRs, SMBIOS (the BIOS version), every ACPI table, `kernel/aml.c` (devices and _HIDs, regions, EC fields, thermal zones, P-states, without running AML), `drivers/ec.c` (the EC, reads only), SMART in `drivers/ata.c`. Tested in QEMU with a test SSDT (`tests/kernel/make-probe-ssdt.py`). **Still to do: run it on the EeePC** (`docs/HARDWARE-TEST.md` §5)
- [ ] (b) Read-only monitoring: kernel sampling (CPU %, clock, temperatures, fan, memory, disk, battery), a timer event for apps, the dashboard
- [ ] (c) History and graphs (RAM, tiered: 1 s / 1 min / 5 min)
- [ ] (d) Alerts with duration conditions, on screen and as a beep
- [ ] (e) Fan control with every safety feature (floors, emergency maximum, sanity checks, hand-back to firmware on exit, crash or lost sensors)
- [ ] (f) Profiles (Silent, Balanced, Performance, custom; AC/battery switching) and power management (SpeedStep, backlight)
- [ ] (g) Top bar indicator and polish

---

## Phase 5 Done Criteria
Per section: it works on the real EeePC, has isolated tests, fails loudly rather than silently, and boot stays within `docs/BOOT-BUDGET.md`.
