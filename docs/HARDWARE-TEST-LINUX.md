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
| Touchpad | The pointer moves smoothly (faster when you move fast); a tap clicks, a two-finger tap right-clicks; the buttons work; **two fingers moving up and down scroll** (try Files, Settings → System's time zones, a web page). If the touchpad misbehaves, see below |
| Keyboard | Open Calculator from the app menu and type `5+3` and Enter (AZERTY: the digits need Shift): it shows 8 |
| A USB mouse | Plug one in: it works at once, **at full speed** (the BIOS slowdown is gone: Linux drives USB itself); its wheel scrolls |
| Apps | Files, Settings (change the style; restart; still changed), Blocksmith (F5 runs the project) |
| Power menu | Shut Down switches the EeePC off; Restart restarts it |
| Wi-Fi | Settings → Network (its tab): your network is listed; Connect, type the password: "Connected", and the Wi-Fi icon lights up in the top bar. Restart: it joins by itself. If the list says Wi-Fi is switched off, press **Fn+F2** |
| Web | With Wi-Fi or Ethernet connected: the app menu's **Web** opens the start page; Wikipedia and a DuckDuckGo search load (no "privacy error": the clock is set from the internet). Pages are in the desktop's font. Note how long a page takes |
| Downloads | In Web, click a link to a file (a .zip or .pdf, under 32 MB): the status bar says "Downloading..." then "Saved /Downloads/..."; Files shows it in Downloads |
| Battery | The top bar shows the battery and its %; unplug the charger: Settings → **System** says "on battery" and, after a minute, how long is left |
| Brightness | Fn+F5 / Fn+F6 dim and brighten the screen; Settings → System's slider moves it too, and it's kept after a restart |
| Time zone | Settings → System: London is chosen and the top bar's clock is right; pick another city, the clock follows |
| Ethernet | Plug in a cable: the Network page says connected, with an address; unplug it: no cable |

**Photo:** the **Log** app (the dock), scrolled to the top, then to these lines:
- `lkx-start: disk /dev/sd... after ... ms` — the stick, found
- `drm: 1024x600 on connector ... (the built-in panel)` — the screen
- every `input: ...` line — the keyboard and touchpad Linux found
- `[boot] desktop t=...`
- every `net: ...` line (which network chips Linux found, and Wi-Fi joining)
- `input: touchpad ...` (its size) and the `machine: ...` lines (backlight, time zone)
- the `linux: ...` lines: Linux's own warnings (anything red-flag looking is worth a photo)

## If something goes wrong
| What you see | What to send |
|---|---|
| Text scrolls and stops, or "Kernel panic" | A photo of the screen |
| A black screen that stays black | Wait a minute, then a photo; then hold the power button |
| The desktop, but no pointer movement or no keys | A photo of the Log's `input:` lines |
| `lkx-start: disk not found` in the Log | Which USB port the stick is in; try another |
| The touchpad jumps, doesn't click or doesn't move | A photo of the Log's `input:` lines. To use it the old way meanwhile (no scrolling): hold **Shift** as the stick starts, type `mouse` at `boot:` and press Enter |
