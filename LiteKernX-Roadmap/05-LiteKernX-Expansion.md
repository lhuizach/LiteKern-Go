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
- [ ] Persistent settings: save the Phase 3 §5 theme choices. Needs a filesystem decision first (the disk driver and filesystem are still a v1.0 non-goal)
- [ ] USB keyboard/mouse (HID) only if a real need turns up; the built-in ones are PS/2

## 4. Networking
- [ ] Atheros Ethernet driver (`1969:1026`, found on bus 3 in Phase 1 §3)
- [ ] Network stack: IPv4, ARP, DHCP, UDP, then TCP
- [ ] Socket syscalls for KERN86 apps, added only as an app needs them
- [ ] Wi-Fi (`168c:002a`) last, if at all: it needs firmware, 802.11 and WPA, and is by far the biggest item in this phase

## 5. Multiple users
- [ ] User accounts, login screen, and per-user settings (needs §3's writable storage)
- [ ] File permissions, if a filesystem exists by then

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
