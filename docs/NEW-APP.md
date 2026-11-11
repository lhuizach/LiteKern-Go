# Making an app for LiteKern Go

An app is one folder in `apps/` with some C and a `kerns.json`. This page is the whole path from nothing to an app on the phone. For the API, see [APPS.md](APPS.md) (an example, the calls grouped, the limits) and `kernel/kern86_abi.h` (every call). Everything below runs in WSL, from the project folder.

## The five steps

| Step | Command | What it does |
|---|---|---|
| 1. Start | `bash tools/new-app.sh NAME "Name on the screen"` | Makes `apps/NAME/` from a small working app (a window, a counter, a button, a saved file), with its `kerns.json`. Nothing else to register. |
| 2. Write | edit `apps/NAME/NAME.c` | The template has comments for each part. |
| 3. Check | `bash tools/check-app.sh NAME` | About a minute: builds with `-Wall -Wextra -Werror`, opens the app on the PC desktop, taps its icon, fails if it didn't open or anything crashed. |
| 4. Look | `make -f platform/Makefile run` | The whole phone desktop in a window on the PC (the mouse is the touchscreen). Try it properly here. |
| 5. Phone | `bash tools/push-app.sh NAME` | Builds for the phone and puts it there over USB. Lasts until the phone restarts. |

To keep it for good: `bash tools/build-system.sh` (add `--tests` for the full tests), then `bash phone/update-system.sh --check` and `--write system`. **That writes to the phone's boot partition, so it needs the owner's yes in chat each time.** It never touches the user's data.

## The rules an app lives by

- C only, no `malloc`: static arrays. Whole numbers (no floating point in apps). Up to 1 MiB of code and data and a 16 KiB stack; `k86_memory()` gives a bigger block when needed.
- Apps take turns: go back to `k86_wait_event()` quickly. Use `k86_timer_set()` for animation.
- Colours from `theme_get()`, never fixed, so the app follows the user's style and accent. Redraw on `K86_EVENT_THEME` and `K86_EVENT_RESIZE`. Lay out from the canvas size.
- Touch first: big targets (the theme's `button_size`), drag to scroll or move, portrait. The screen is 720x1280; the window is 720x1224 under the status bar. One finger at a time.
- No blur or glass: the CPU is a Cortex-A53 with software drawing. Draw only what changed and `k86_present()` that part.
- Files: `k86_home()` is the user's own folder; put what the user makes there. `k86_write()` replaces a whole file safely.
- `k86_log()` lines show up in the Log app as `user: ...`; keep them ASCII (the tests read them).
- A game: our own name and our own art, drawn in code. Not a copy of an existing game.

## The `kerns.json`

`name` (under the icon and in the top bar), `icon` (empty: a letter tile), `entry` (`NAME.lkx`), `order` (where it sits: lower first; `new-app.sh` puts a new app after the others), `width` / `height` (`720` x `1224`), `min_width` / `min_height`. The dock (Files, Gallery, Settings) is fixed by name in the shell.

## For another agent working on an app

Give it this page and say what to build. What it may change: **only** `apps/NAME/` (its C files and `kerns.json`). What it must not: `kernel/`, `platform/`, `sdk/`, `linux/`, `phone/`, `tests/`, `tools/`, `docs/`, `assets/`, `CLAUDE.md`, other apps. If it thinks one of those needs a change, it says so in its report and leaves it. It never flashes or boots the phone (`heimdall`, `fastboot`, `odin`), never uses `sudo`, never downloads anything, never opens `private/`, never pushes to GitHub. It finishes with `bash tools/check-app.sh NAME`, and the full test suite (`bash tests/linux/test-linux.sh`) still passing, and commits only its own folder.

## When something's wrong

- The check says it didn't open: the app's name in `kerns.json` has to match what's on the home screen; run the full desktop (step 4) and look.
- `push-app.sh` can't reach the phone: it must be running our system and plugged in; `ssh -i ~/.ssh/litekern-phone root@172.16.42.1` is the test.
- A new app's icon isn't there after pushing: `push-app.sh` restarts the desktop for a new app; give it a few seconds. Open apps are closed by that.
- The phone won't boot after a system update: power off, then on holding Volume Down (lk2nd), `fastboot boot` a known good image (`Apps for zach/to-fedora/boot-litekern-goN.img`), and run `phone/update-system.sh` from that. The data was never touched.
