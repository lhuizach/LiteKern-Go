# LiteKern X on Linux — Testing on the EeePC 1000HE

The Linux port's stick (LiteKernX-Roadmap/06, phase L3 onwards). Same EeePC, same stick, same way of writing it as [HARDWARE-TEST.md](HARDWARE-TEST.md); what's on it is new: Linux underneath, your desktop and apps on top.

## 1. Build the image
In the project folder (`C:\Users\lhuir\LiteKern-X-Linux`), from PowerShell:
```
wsl bash linux/build.sh
```
The first time takes 1–2 hours (Buildroot builds its compiler and Linux); after a change to the desktop or the apps, `wsl bash linux/build.sh image` takes minutes. It makes **`build-linux\litekernx-linux.img`** (65 MiB).

## 2. Write it to the stick, boot the EeePC
As before ([HARDWARE-TEST.md](HARDWARE-TEST.md) §2 and §3): Rufus or Etcher, DD image mode, the whole stick is erased; then **Esc** at the ASUS logo and pick the stick.

What you'll see: a black screen for a few seconds while Linux starts (no text on purpose), then LiteKern X's splash, then the desktop. Settings and users start fresh (the stick was rewritten).

## 3. What to check and photograph
| Check | What should happen |
|---|---|
| Boot | The desktop appears, filling the screen (1024x600, no black bars). Note roughly how many seconds from the ASUS logo to the desktop |
| Touchpad | The pointer moves smoothly; tapping clicks (as on the kernel version) |
| Keyboard | Open Calculator from the app menu and type `5+3` and Enter (AZERTY: the digits need Shift): it shows 8 |
| A USB mouse | Plug one in: it works at once, **at full speed** (the BIOS slowdown is gone: Linux drives USB itself) |
| Apps | Files, Settings (change the style; restart; still changed), Blocksmith (F5 runs the project) |
| Power menu | Shut Down switches the EeePC off; Restart restarts it |

**Photo:** the **Log** app (the dock), scrolled to the top, then to these lines:
- `lkx-start: disk /dev/sd... after ... ms` — the stick, found
- `drm: 1024x600 on connector ... (the built-in panel)` — the screen
- every `input: ...` line — the keyboard and touchpad Linux found
- `[boot] desktop t=...`

## If something goes wrong
| What you see | What to send |
|---|---|
| Text scrolls and stops, or "Kernel panic" | A photo of the screen |
| A black screen that stays black | Wait a minute, then a photo; then hold the power button |
| The desktop, but no pointer movement or no keys | A photo of the Log's `input:` lines |
| `lkx-start: disk not found` in the Log | Which USB port the stick is in; try another |
