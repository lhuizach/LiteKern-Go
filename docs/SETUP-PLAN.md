# LiteKern Go: first-run setup (plan, 23 Nov 2026)

A plan, not built. What the phone asks the first time it starts (and after a factory reset), so a new owner
gets from "boot animation" to "my home screen" without touching a config file or ssh.

## What other systems ask

Sources: carrier setup guides for Android and iPhone, GNOME's design page for its Initial Setup
(`wiki.gnome.org/Design/OS/InitialSetup`), and Purism's mobile build of it (Librem 5 / postmarketOS). The exact
order differs by version; the pattern is the same everywhere.

| Step | Android | iOS | GNOME / Linux phones |
|---|---|---|---|
| Language (then region) | first | first ("Hello") | first (language, formats) |
| Copy from an old device | yes (cable, QR) | yes (Quick Start) | no |
| Wi-Fi | yes, skippable | yes | yes (mobile build: first network) |
| Account sign-in | Google (skippable) | Apple ID (skippable) | Online accounts (skippable); a local user is always made |
| Screen lock | PIN (4+ digits), then fingerprint | passcode, Face ID | password |
| Time zone and clock | automatic | automatic | page of its own |
| Privacy and data sharing | several switches | analytics prompts | one privacy page |
| Assistants, payments, Screen Time | yes | yes | no |
| Theme / look | at the end, light or dark | at the end, appearance | none |

What matters from this: every system starts with language, then network, then who you are and how it locks, and
finishes with a few small choices. Everything that can be done later is skippable. Nobody asks for more than one
screen of anything.

## What LiteKern Go already has (so the setup only has to start them)

- A PIN lock screen and password accounts (`SYS_USERS`, `SYS_USER_CHANGE`, up to 8 users, admin or not).
- Wi-Fi: scan and join (`SYS_NET_SCAN`, `SYS_NET_JOIN`).
- Time zone, brightness, volume, screen-saver time (`SYS_TIMEZONE_SET` and friends), all saved in `machine.txt`.
- Appearance: dark or light, accent colour, wallpaper (`SYS_APPEARANCE_SET`), and the lock screen's clock style.
- Home screen layout and app names (`settings.txt`, `home-*`).
- An on-screen keyboard and number pad (`SYS_WINDOW_KEYBOARD`).
- Not present: language (the UI is English only), regions, any account service (no Google or Apple equivalent),
  copy from another device, fingerprint (the A5 has no sensor; its home button is only a key).

## What LiteKern Go's setup asks (proposal)

Seven screens, each one question, full screen, big touch targets, a progress dot row, Next always at the
bottom, Back always at the top left. No step is mandatory except 3 and 5.

1. **Welcome.** The logo and "Hello" in a few languages in turn (the boot animation's style), one button:
   *Get started*. A small "Emergency" or "Accessibility" corner is not needed here.
2. **Language and keyboard.** English (UK) for now; the screen exists so adding languages later is not a redesign.
   Keyboard layout: UK or US.
3. **Wi-Fi.** The scan list from `SYS_NET_SCAN`, password via the on-screen keyboard, *Skip for now* at the bottom.
   Needs the network for step 4's clock check only if the clock is wrong.
4. **Date, time and time zone.** Pre-filled from the clock; if there is no network and the date is before the build
   date, it asks for the date and time by hand (the phone has no trustworthy clock without them). Time zone picker
   (`SYS_TIMEZONE_SET`).
5. **You.** Name, then a PIN (4 to 8 digits, typed twice, number pad). This creates the first user as an admin
   (`K86_USER_ADD`). The PIN screen explains in one line that it locks the screen; *Skip* is allowed and means no lock
   (the same as today's default), with a reminder in Settings.
6. **Look.** Dark or light (two big cards showing the real home screen), an accent colour row, a wallpaper row. Live
   preview behind the cards.
7. **Done.** "You're all set", a short list of three things to try (swipe up for apps, hold the home screen to edit,
   hold Power for the power menu), button: *Start*. This opens the home screen with the unlock slide.

Left out on purpose: sign-in accounts (nothing to sign in to; R3lay and Weather keep their own settings in their
apps), copy from old phone, voice assistant, payments, analytics and data sharing (the phone sends nothing
anywhere), parental controls (the owner sets up their brother's phone), and fingerprint.

Maybe later, after the plan is in use: a language list, an "Apps" page that offers the optional extras (Browser
needs the 3 GiB extras area, so it says so), and a "restore my photos and settings from a file" step using the data
disk's export.

## How it works inside

- A new `kernel/setup.c` in the shell, drawn like the lock screen (full screen, own event handling, no windows),
  started by `desktop_start` before the lock screen when `setup-done` is missing from `settings.txt`.
- State is one line, `setup-done 1`, written at the very end, so a power cut part way through repeats the whole
  setup next time and never leaves a half-made user. Each step writes its own setting as it goes (Wi-Fi, zone,
  appearance), the user is created in step 5 and kept.
- It reuses what exists: the Wi-Fi list code from Settings, the time zone list, the PIN pad and `SYS_USER_CHANGE`
  from the lock screen and Settings, the appearance cards from Settings. Where a piece lives only inside Settings
  (an app, not the shell), it moves into a shared shell file first (`kernel/wifi_ui.c` and similar) rather than
  being copied.
- Re-running it: Settings > About > *Run setup again* (asks for the PIN, then clears `setup-done` and restarts the
  shell). The same switch is what a factory reset sets.
- Tests: `tests/linux/` starts the shell with no `setup-done`, taps through all seven screens by coordinates (as the
  home tests do), and checks that `settings.txt` and the user list are right and that the next start goes straight
  to the lock screen. Existing tests need `setup-done 1` pre-seeded in their disk image so they keep starting at
  the home screen.

## Order of work

1. Move the Wi-Fi list and the PIN pad out of the app/lock code into shared shell code, so the setup and Settings
   share them (no new behaviour).
2. The setup shell: screens 1, 2, 4, 7 (no new hardware or services; a skeleton you can click through on the PC).
3. Screens 3, 5, 6 (the parts that call the system); the `setup-done` flag; the test.
4. On the phone: first start after `update-system.sh --write system` with the flag cleared by the tool (`--reset-setup`
   in `phone/update-system.sh`, so a flash does not silently skip it).
5. Run-again in Settings.

Open questions for the owner:

- Is the PIN step required in this phone's first setup (the brother uses it daily; a lock on by default may annoy
  him), or offered with the default "no lock"?
- Languages beyond English: wanted at all?
- Should the first user be named after the brother (his name on the lock screen), or is the name only for the
  "Hello, Name" line?
- Do we want a short walkthrough of gestures (step 7), or is one screen of three tips enough?

Sources: carrier setup guides for Android and iPhone (att.com device support), Tech Advisor's Android quick
start, GNOME's Initial Setup design page, Purism's initial setup docs, postmarketOS's mobile gnome-initial-setup
merge request.
