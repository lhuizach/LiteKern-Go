# A browser with real JavaScript (plan, 11 Nov 2026)

Not started. The idea: WPE WebKit from Alpine's ready-made packages, in a small Alpine sandbox on the phone's spare storage, run by our own app `lkx-web` that draws its pages into a LiteKern window. No long WebKit compile. (The other design, in `ROADMAP.md` and `linux/buildroot-web.fragment`, builds WPE WebKit with Buildroot: 3 to 6 hours on the PC, 150 to 250 MB; kept as the fallback. NetSurf, `linux/package/netsurf-lkx`, is the light fallback without real JS.) Firefox is out: it needs X11 or Wayland and a GPU, and our desktop is neither.

Alpine has WPE WebKit for aarch64: `wpewebkit` 2.40.5 on 3.19, newer on edge (pkgs.alpinelinux.org).

## Phases (each ends in a decision)

0. **Prove the design on the PC** (no phone). Alpine x86_64 base (official site, hash checked) in WSL; `apk add wpewebkit`; a minimal `lkx-web` showing a page in the headless desktop. Answers: does WPE render without a GPU (Mesa software), what it needs (fonts, certificates), how it meets our app protocol. If it can't render: stop and look at NetSurf or the Buildroot WebKit.
1. **Room on the phone.** An "extras" area, about 3 GB at the end of `userdata` after the data disk (which ends at sector 4343808; about 9 GB is unused): ext4, labelled, mounted by `lkx-start` like the boot partition. A new `phone/update-system.sh --write extras`. First write to the phone for this: ask the owner.
2. **Alpine on the phone.** Alpine's aarch64 base into the extras area, then `apk add wpewebkit` inside it on the phone over Wi-Fi (no cross-building, no admin rights on the PC). Needs the owner's yes for the downloads.
3. **Speed test on the phone.** A simple page and a JavaScript-heavy one: memory and speed. Decide whether to go on.
4. **The app `lkx-web`** (built inside the sandbox): WebKit's view into our window (libwpe and wpebackend-fdo hand over the frames), touch as taps and scrolling, the on-screen keyboard, back / forward / reload, a URL bar. The big part: 2 to 4 days.
5. **Tidy up:** icon and manifest, a memory limit, updates, Settings, tests.

## Unknowns
- Software rendering (llvmpipe) speed on a Cortex-A53: the biggest risk, tested in phases 0 and 3.
- WebKit's own sandbox may not work in a chroot: it would be turned off (acceptable on a private phone).
- No video decoding (YouTube at 720p won't play); about 300 to 500 MB of RAM for one page on a 2 GB phone.

## What gets written to the phone
Only the extras area, through the update tool, each time with the owner's yes. No flashing commands. Nothing in the boot partition or the data disk changes. Every download (Alpine's base, packages) is asked first.

## Why not a GPU build / Kaggle
Compilers don't run on GPUs. The PC (i5-11400F, 12 threads) beats Kaggle's free 4 cores, and Kaggle's 20 GB of saved output is too small for a WebKit build tree.
