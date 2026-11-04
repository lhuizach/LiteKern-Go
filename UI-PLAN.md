# LiteKern Go — UI and UX plan

How the phone looks and behaves, and how we get there from LiteKern X's
desktop shell. Built and tested in the VM first (see "Testing in the VM").

## Principles

- **A normal phone.** Anyone who has used an Android phone should know what to
  do without being told. No desktop habits: no windows, no pointer, no hover.
- **LiteKern X's look.** Same Adwaita-based theme (dark/light, accent colours),
  same fonts, icons, wallpapers and springy animations, sized for a finger.
- **The hardware keys do the navigating.** The A5 has Back, Home and Recent apps
  keys under the screen, so there is **no on-screen navigation bar or
  taskbar**. All 1280 px of height go to the status bar and the app.
- **Light on memory.** 2 GB total. One app on screen at a time, background apps
  paused, nothing animated that isn't being looked at.
- **Fast to respond.** A tap shows feedback within one frame, even if the
  action takes longer.

## The hardware keys

Under the screen, left to right: **Back** (touch key), **Home**
(physical button), **Recent apps** (touch key). On the side: Power, Volume Up and
Volume Down.

| Key | Short press | Long press (500 ms) |
|---|---|---|
| Home | Go to the home screen. On the lock screen: wakes the screen | Quick settings |
| Home (screen off) | Wake to the lock screen | — |
| Recent apps | Open the app switcher, or close it again | — |
| Back | Close the keyboard, then any open panel/dialog, then the app's previous page, then go home | — |
| Power | Screen off (and lock). Screen on when off | Power menu: Power off, Restart, Lock |
| Volume Up/Down | Volume pop-up and change. In the camera: take a photo | Keeps changing |
| Power + Volume Down | Screenshot (saved to Pictures/Screenshots) | — |

Back is passed to the app first. The app says whether it handled it (closed a
page); if not, the shell does the default (closes the app, goes home).

The touch keys have backlights. They turn on with any touch and off after a
few seconds, like on stock Android.

**Unverified:** what key codes Linux gives these keys on the A5. Expected:
`KEY_HOMEPAGE` (Home), `KEY_BACK` (left key), `KEY_APPSELECT` or `KEY_MENU` (right key),
`KEY_POWER`, `KEY_VOLUMEUP/DOWN`. Check with `evtest` on the first Nura boot,
then put the real codes in the key map. The touch-key backlight driver is also
unverified.

## Sizes

The screen is 720x1280 on 5 inches, about 294 pixels per inch. LiteKern X was
sized for a 1024x600 netbook at about 118 ppi, so everything gets bigger.

- **UI scale:** 2x LiteKern X's sizes as the starting point, tuned on the real
  screen. One `theme.scale` value so it can be changed in one place.
- **Touch targets:** at least 88 px (about 7.5 mm), with 16 px between targets.
- **Text:** body 30 px, secondary 26 px, titles 40 px, the lock screen clock
  about 180 px.
- **Margins:** 32 px at the screen edges.

## Screen layout

```
┌──────────────────────────┐
│ 12:45        ▼ Wi-Fi ▮80%│  status bar, 56 px, always there (except full-screen games)
├──────────────────────────┤
│                          │
│                          │
│     the app, or the      │  1224 px for content
│      home screen         │
│                          │
│                          │
│                          │
└──────────────────────────┘
    [Back]   (Home)  [Recent]    hardware keys, not drawn
```

## Screens

### Boot splash

The LiteKern Go logo (the blue G with a grey bar, `tools/make-logo.py`)
and "LiteKern Go" under it, centred on black. It fades in, stays at least
1.5 s, then cross-fades into the lock screen or the home screen
(`kernel/splash.c`). No text scrolling past: Linux's messages go to the
serial port only, and the VM boots the kernel straight from EFI, so no
bootloader text either.

On the phone, before it: Samsung's own screen ("Samsung Galaxy A5",
"Powered by Android") stays, as it's the untouched bootloader's; Android's
"SAMSUNG" boot animation goes. **Unverified:** whether lk2nd shows anything
in a normal boot, the black gap before our logo, and Samsung's yellow
"not SEANDROID enforcing" text after a custom boot image is flashed.

### Lock screen

- Wallpaper, dimmed. Big clock and the date in the top third.
- Notifications as cards under the clock (title only, no content, for privacy).
- **Swipe up anywhere** to unlock. If a PIN is set, the swipe shows a number
  pad (large keys, dots for digits entered). Wrong PIN: the dots shake.
- Battery percentage while charging.
- Reuses `lockscreen.c`. The user picker and power icon go away; the power menu
  moves to a long press of the Power key.
- Users: one user only on the phone. The multi-user code stays but is hidden.

### Status bar

- Left: the time. Then small notification icons (up to 4, then a dot).
- Right: Wi-Fi strength, battery icon and percentage. A charging bolt when
  plugged in. Later: a mobile signal icon if calls ever work.
- Its colour follows the app's header bar so they look like one piece. Over
  the home screen it is transparent on the wallpaper.
- **Pull down** from it (or long-press Home) to open the quick settings panel.

### Quick settings panel

Slides down from the top over the current screen, dims what's behind.

- Top: date and time, a Settings gear (opens Settings).
- **Tiles** (2 rows of 4, round, tap to toggle, long press opens that page in
  Settings): Wi-Fi, Bluetooth (if it works), Flashlight (if the LED works),
  Rotation lock (if rotation is ever added), Dark mode, Do not disturb,
  Airplane mode, Screenshot.
- **Sliders:** brightness (**unverified on the A5**: reported as a gap), and
  volume.
- **Notifications** under the tiles: tap opens the app, swipe sideways
  dismisses, "Clear all" at the bottom.
- Close it with a swipe up, Back, Home, or a tap on the dimmed area.

### Home screen

No dock, no taskbar.

- Wallpaper behind everything.
- Top: a clock and date widget (the brother can turn it off in Settings).
- **A grid of app icons**, 4 columns, 5 rows a page. Icon 112 px, the name
  under it in one line (cut with "…" if long).
- **Swipe left/right** between pages, dots above the bottom edge show the page.
- **The last page is "All apps":** an A to Z list of every installed app, so
  nothing is lost even if it was removed from the home pages.
- **Long press an icon:** it lifts and can be dragged to another spot or page.
  A small menu offers "Remove from home" and "App info". Removing from home
  does not uninstall.
- **Long press empty space:** Wallpaper, Home settings.
- New apps are added to the first free spot.
- Reuses the app grid in `desktop.c` (draw_grid, tiles), without the dock and
  top bar.

### Apps

- **Always full screen** under the status bar. No window borders, no moving or
  resizing. The window manager keeps one app visible at a time.
- **Header bar** at the top of each app (the LiteKern X header bar, taller): the
  title, and the app's own buttons on the right. No back arrow in the header,
  because Back is a hardware key. Pages deeper than the first may still show
  one for clarity (decide when we build Settings).
- **Opening an app:** the icon zooms up into the app (about 200 ms). Closing:
  it shrinks back to its icon.
- **Paused in the background:** an app that isn't on screen gets no frames and
  no input. Its process is kept until memory runs low, then the oldest one is
  closed (it shows a fresh start next time). Apps that need to run in the
  background (music, downloads) can ask for it later; not in v1.
- **Full-screen games:** an app can hide the status bar. A swipe down from the
  top edge shows it for a few seconds.
- **Portrait only** in v1. Landscape (for games and photos) needs the
  accelerometer, which is a later job.

### App switcher (Recent apps key)

- The open apps as large cards (a snapshot of each), newest on the right,
  scrolling sideways. The current app's card is centred.
- Tap a card: switch to it.
- **Swipe a card up:** close that app (frees its memory).
- "Close all" button at the bottom.
- Empty: a short message, "No recent apps".
- Recent apps, Back or Home leave it.

### Dialogs, menus and pop-ups

- Dialogs sit centred, with rounded corners and big buttons in a row at the
  bottom (the main action on the right, in the accent colour).
- Menus open as a **sheet from the bottom** of the screen with large rows,
  rather than small drop-downs next to the finger.
- Short messages ("Screenshot saved") show as a **toast** above the bottom
  edge for 2 seconds.
- Back closes any of them.

### Power menu

Long press Power: a sheet with large buttons: **Power off**, **Restart**,
**Lock**. Dimmed background, Back to cancel.

### Volume pop-up

A slim slider on the right edge near the volume keys. Disappears 2 seconds
after the last press.

## On-screen keyboard

Needed for Wi-Fi passwords, search, names, the calculator's text fields.

- **Appears** when a text field gets focus. It pushes the app's content up so
  the field stays visible. Back or tapping outside the field hides it.
- **Layout:** UK QWERTY, 4 rows plus a bottom row:
  - `q w e r t y u i o p`
  - `a s d f g h j k l`
  - `⇧ z x c v b n m ⌫`
  - `123  ,  space  .  ⏎`
- Keys about 66x100 px, 8 px gaps. Height about 470 px, a bit over a third of
  the screen.
- **Shift:** one tap for one capital, double tap for caps lock. Auto-capital at
  the start of a sentence.
- **123** switches to numbers and symbols (two pages). Number-only fields
  (PIN, calculator) open a big number pad instead.
- **Key preview:** the letter pops up above the finger while pressed.
- **Hold ⌫** to delete repeatedly, faster after a second.
- **Hold a letter** for accents (é, ü...) and emoji are not in v1.
- Typing sounds and vibration: off by default, an option later if the
  vibration motor works (**unverified**).
- No word suggestions or swipe typing in v1.
- Built into the shell (not an app), so it works in every app and on the lock
  screen. Apps get the typed text as normal key events, so existing LiteKern X
  apps work unchanged.

## Touch and gestures

The rules every screen follows:

- **Tap:** finger down and up within 300 ms, moved less than 24 px. Activates
  on release, not on press, so scrolling a list never opens things by accident.
- **Pressed look:** shows on press (the `button_active` colour), cancelled if the
  finger starts scrolling.
- **Long press:** 500 ms without moving 24 px. A short vibration if the motor
  works.
- **Drag to scroll:** lists and pages follow the finger 1:1, then keep moving
  with momentum after a flick and slow down. At the end, a soft stretch
  (overscroll) that springs back. Uses the existing spring code.
- **Edge swipes:** only from the top (status bar / quick settings). No side or
  bottom edge gestures, so they don't fight the apps or the keys.
- **One finger in v1.** Pinch to zoom (photos) needs multitouch, which comes
  with the real touchscreen.
- **No hover states,** no tooltips, no pointer drawn. The mouse cursor code stays
  for the VM and is hidden by default.

## Look and feel

- **Theme:** LiteKern X's Adwaita dark (default) and light styles, same 6 accent
  colours. Everything still reads `theme_get()`, just with bigger sizes.
- **Wallpapers:** LiteKern X's set, cropped to portrait. The brother's own
  photos can be set as wallpaper (from Gallery or Settings).
- **Fonts:** the same as LiteKern X, at the sizes above.
- **Icons:** LiteKern X's app icons, redrawn or scaled to 112 px.
- **Animations:** the existing springs. About 150-250 ms, never blocking input:
  a tap during an animation acts right away.
- **Rounded corners:** cards, dialogs, sheets and tiles. Larger radius than on
  the desktop (about 24 px).

## Apps for v1

| App | From | Notes |
|---|---|---|
| Settings | LiteKern X | Rework for portrait: one list, pages slide in |
| Files | LiteKern X | Rework for portrait, long press for actions |
| Calculator | LiteKern X | Bigger buttons, portrait layout |
| Blocksmith | LiteKern X | Check it plays with touch |
| Clock | new | Alarms, timer, stopwatch. Alarms need the phone to wake from sleep (**unverified**) |
| Gallery | new | Photo grid, full-screen viewer, swipe between photos |
| Camera | new | Only if the camera works on the A5 (**the biggest risk**) |
| Games | ports | Native Linux games, picked with the brother |
| Hardware Probe | LiteKern X | Remove (Eee PC only) or turn into an "About phone" page |

### Settings pages

Wi-Fi, Bluetooth (if it works), Display (brightness, dark mode, accent, screen
timeout, wallpaper), Sound (volume), Lock screen (PIN, show notifications),
Home screen (clock widget, grid size), Storage (internal, SD card), Battery,
Date and time, About phone (model, version, memory, kernel).

## From LiteKern X's shell to LiteKern Go's

What changes in the code, roughly in build order:

1. **Theme sizes.** Add a scale to `theme.c`, double the sizes. Everything gets
   bigger at once. Check what breaks.
2. **Key map.** Map the phone keys (Home, Back, Recent apps, Power, volume) in
   `keymap.c` and `platform/devices.c`, send them to the shell. In the VM, use
   PC keys for them (see below).
3. **Full-screen window manager.** `wm.c`: every window maximised under the
   status bar, no title bar drag or resize, one visible at a time, a stack of
   recent apps. Pause apps that aren't visible.
4. **Home screen.** `desktop.c`: remove the dock and top bar, keep the grid,
   add pages and the All apps page.
5. **Status bar.** Replace the desktop top bar with the phone status bar.
6. **Back.** Add a Back event to the app API (KIP / `kern86.h`), with the
   "handled" reply, and the default behaviour in the shell.
7. **Touch rules.** Tap/long press/drag recognition in one place in the shell,
   momentum scrolling in the widget lists.
8. **On-screen keyboard.**
9. **Quick settings and notifications.**
10. **App switcher.**
11. **Lock screen** changes (swipe to unlock, PIN pad).
12. **Apps** reworked for portrait, then the new ones.
13. **Power, volume, screenshot** handling and pop-ups.

After step 5 the VM already looks and works like a phone. Each step gets a
test in `tests/linux/` where it can be checked from the serial log.

## Testing in the VM

The VM's mouse is the finger (USB tablet: click = tap, drag = swipe). PC keys
stand in for the phone's:

| Phone | VM key |
|---|---|
| Home | `Home` |
| Back | `Esc` |
| Recent apps | `F1` |
| Power | `F12` |
| Volume Up / Down | `PgUp` / `PgDn` |

Can't be tested in the VM: real touch feel, multitouch, the touch keys' real
codes and backlight, brightness, vibration, speed on the Snapdragon 410.

## Open questions

- **What extras the brother wants.** They may add apps or home screen features.
- **Camera:** works or not? Decides whether Camera and Gallery's "take photo"
  exist.
- **Games:** which native Linux games, and do they need landscape?
- **PIN lock:** wanted, or swipe only?
- ~~**Name and icon** for the phone on the lock and boot screens~~: "LiteKern Go", the G logo (decided).
