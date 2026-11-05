#!/usr/bin/env bash
# LiteKern X on Linux (LiteKernX-Roadmap/06, phases L1 and L2): lkx-shell, the
# desktop as a Linux program, and the apps as Linux processes, run headless with scripted input (platform/headless.c,
# the same key language as tests/kernel) at LiteKern Go's 720x1280 (the phone's
# screen; the mouse is the finger). Checks its log, like tests/kernel/test-kernel.sh,
# and its screenshots.
#
# Usage (in WSL): bash tests/linux/test-linux.sh
set -uo pipefail
cd "$(dirname "$0")/../.."

failures=0
W=720 H=1280
# The shell and the apps, the tests' misbehaving ones too; their disk image
# (the kernel build's, with them in the ramdisk).
TEST_APPS="tests/apps/crash tests/apps/badcalls tests/apps/hang tests/apps/notify"
make -s -f platform/Makefile EXTRA_APPS="$TEST_APPS" >/dev/null || exit 1
rm -f build/test-apps/litekernx.img build/test-apps/mbr.bin     # FAT_MB isn't a make dependency
make -s BUILD=build/test-apps FAT_MB=64 "EXTRA_APPS=$TEST_APPS" build/test-apps/litekernx.img >/dev/null || exit 1
for t in wm gfx widgets; do
    make -s -f platform/Makefile SELFTEST=$t LBUILD=build-linux/selftest-$t >/dev/null || exit 1
done
make -s -f platform/Makefile KBUILD=build/test-saver LBUILD=build-linux/saver SCREENSAVER=2 >/dev/null || exit 1

mkdir -p build-linux/test
cp build/litekernx.img build-linux/test/disk.img

# Two users on a copy: User (an admin, PIN 1234) and Ann (no
# password), written as kernel/users.c writes them (salted SHA-256, 20000 rounds).
cp build/litekernx.img build-linux/test/users.img
python3 - build-linux/test/users.txt <<'EOF'
import hashlib, sys
def h(salt, pw):
    d = hashlib.sha256(salt + pw).digest()
    for _ in range(20000 - 1):
        d = hashlib.sha256(d + salt + pw).digest()
    return d.hex()
salt = bytes(range(16))
open(sys.argv[1], "w").write(
    "# test users\n"
    f"user admin 3584e4 {salt.hex()} {h(salt, b'1234')} User\n"
    f"user standard e45c84 {bytes(16).hex()} - Ann\n")
EOF
mmd -i build-linux/test/users.img@@8388608 ::/LiteKern 2>/dev/null
mcopy -o -i build-linux/test/users.img@@8388608 build-linux/test/users.txt ::/LiteKern/users.txt || exit 1

# run BINARY DISK "STEPS" [more lkx-shell args] -> $out: the log, plus the
# screenshot's "screen: ..." lines when SCREEN="checks" (tests/lib/qemu_monitor.py)
run() {
    local bin=$1 disk=$2 steps=$3 shot=build-linux/test/shot.png
    shift 3
    rm -f "$shot"
    out=$(timeout 60 "$bin" --backend headless --disk "$disk" --size ${W}x$H --for 50 \
          --keys "$steps" --shot "$shot" "$@" 2>&1 < /dev/null)
    if [ -n "${SCREEN:-}" ]; then
        out+=$'\n'$(python3 tests/lib/qemu_monitor.py check "$shot" $SCREEN 2>&1 || echo "screen: no screenshot")
    fi
}

# check NAME REGEX...: each must match a line; one starting with ! must match none.
check() {
    local name=$1 re ok=1
    shift
    for re in "$@"; do
        if [ "${re:0:1}" = "!" ]; then
            grep -Eq "${re:1}" <<<"$out" && ok=0
        else
            grep -Eq "$re" <<<"$out" || ok=0
        fi
    done
    if [ $ok -eq 1 ]; then
        echo "PASS  $name"
    else
        echo "FAIL  $name"
        failures=$((failures + 1))
        sed '/^$/d; s/^/      /' <<<"$out"
    fi
}

# Pointer paths, as in tests/kernel: absolute positions turned into moves.
path() { PX=$((W / 2)) PY=$((H / 2)) P=""; }
go() { P+=" move:$(( $1 - PX )),$(( $2 - PY ))"; PX=$1 PY=$2; }
click() { P+=" sleep:0.2 press:1 release"; }
add() { P+=" $*"; }
# The home screen (kernel/desktop.c): apps in 4 columns of 168 x 184 px tiles
# from y 276. tap_app N taps the N-th (0: Files, 1 Calculator, 2 Settings,
# 3 Clock, 4 Blocksmith, 5 Gallery, 6 Task Manager, then Log; the tests' apps come before Log).
tile_x() { echo $(( (W - 4 * 168) / 2 + 84 + ($1 % 4) * 168 )); }
tile_y() { echo $(( 56 + 220 + 72 + ($1 / 4) * 184 )); }
tap_app() { go "$(tile_x "$1")" "$(tile_y "$1")"; click; add sleep:1.5; }
PLAIN=202634

SHELL_BIN=build-linux/lkx-shell

SCREEN="corner=" run $SHELL_BIN build-linux/test/disk.img "sleep:1"
check "starts like the kernel: the disk image's ramdisk and FAT32, the apps, the wallpaper" \
    '^LiteKern X \(on Linux\)$' \
    '^dev boot0 driver=linux-disk bound$' \
    '^storage: boot0 partition at 16384 \(FAT32\): FAT32, read/write$' \
    '^ramdisk: [0-9]+ files' \
    '^lkx: Files \(apps/files/files.lkx' \
    '^lkx: Blocksmith \(apps/blocksmith/blocksmith.lkx' \
    '^wallpaper: crossing, ' \
    '^desktop: ready$' \
    '^screen: 720x1280$' \
    '!^screen: corner '$PLAIN \
    '!PANIC|Segmentation'

# The home screen: swiped left, the All apps page; Home turns back to the first.
path; go 600 800; add press:1; go 500 800; go 300 800; go 150 800; add release sleep:1 phone_home sleep:1
SCREEN="has=ffffff@0,56,720,276" run $SHELL_BIN build-linux/test/disk.img "$P"
check "the home screen: its clock, a swipe to All apps, Home back to the first page" \
    '^home: page 2 of 2 \(All apps\)$' \
    '^home: page 1 of 2$' \
    '^screen: has ffffff in 0,56,720,276: yes$'

# Log from its home screen icon: full-screen under the status bar; Home hides it.
path; tap_app 7; add phone_home sleep:1
SCREEN="corner=" run $SHELL_BIN build-linux/test/disk.img "$P"
check "an app opens full-screen from its home screen icon; Home goes back home" \
    '^desktop: open Log$' \
    '^wm: open Log at 0,56 720x1224$' \
    '^anim: open Log' \
    '^desktop: minimise Log$' \
    '!^screen: corner '$PLAIN

# Power held opens the power menu (a sheet in the middle); its Lock (the third button, 360,760), then a key unlocks.
path; add keydown:phone_power sleep:0.8 keyup:phone_power sleep:0.3; go 360 760; click; add sleep:1.5 spc sleep:1.5
run $SHELL_BIN build-linux/test/disk.img "$P"
check "the power menu locks; a key unlocks" \
    '^shell: lock$' \
    '^anim: lock, ' \
    '^lock: locked \(wallpaper desktop\)$' \
    '^lock: unlocked$'

run $SHELL_BIN build-linux/test/disk.img "sleep:0.5 meta_l-l sleep:1.5 shot:build-linux/test/lock.png"
SCREEN="has=ffffff@150,320,580,460" run $SHELL_BIN build-linux/test/disk.img "sleep:0.5 meta_l-l sleep:1.5"
check "Super+L locks: the big clock" \
    '^shell: lock \(Super\+L\)$' \
    '^screen: has ffffff in 150,320,580,460: yes$'

# LiteKern Go's phone keys (UI-PLAN.md): Home, Recent apps, volume; Power
# locks, held it opens the power menu instead.
run $SHELL_BIN build-linux/test/disk.img "sleep:0.5 vol_up vol_down phone_home phone_recents sleep:0.3 phone_power sleep:1"
check "the phone keys: Home, Recent apps, volume; Power locks" \
    '^shell: volume up, 70%$' \
    '^shell: volume down, 60%$' \
    '^shell: Home$' \
    '^desktop: show desktop$' \
    '^shell: Recent apps$' \
    '^desktop: no recent apps$' \
    '^shell: lock \(Power\)$' \
    '^lock: locked'
run $SHELL_BIN build-linux/test/disk.img "sleep:0.5 keydown:phone_power sleep:0.8 keyup:phone_power sleep:0.5"
check "holding Power opens the power menu, and doesn't lock" \
    '^shell: power menu \(Power held\)$' \
    '!shell: lock \(Power\)'

# The power menu's sheet: Power off, Restart, Lock; a tap outside closes it.
path; add keydown:phone_power sleep:0.8 keyup:phone_power sleep:0.3; go 360 200; click; add sleep:0.5
run $SHELL_BIN build-linux/test/disk.img "$P"
check "the power menu: a tap outside closes it" \
    '^shell: power menu open$' \
    '^shell: power menu closed$' \
    '!shell: lock'

# Volume Up held: it keeps going up (60 -> 100 and stays); the pop-up on the
# right edge (its accent fill, 632..696 x 216..656), gone 2 s after the last press.
SCREEN="has=3584e4@632,216,696,656" \
    run $SHELL_BIN build-linux/test/disk.img "sleep:0.5 keydown:vol_up sleep:1.2 keyup:vol_up sleep:0.3"
check "Volume Up held: the level climbs to 100, the pop-up shows it (the blue accent)" \
    '^shell: volume up, 70%$' \
    '^shell: volume up, 100%$' \
    '^screen: has 3584e4 in 632,216,696,656: yes$' \
    '!shell: volume up, 110%'

# Screenshots: Power, then Volume Down (and the other way round): a PNG in
# /Pictures/Screenshots, no lock, no power menu; a toast says so.
cp build/litekernx.img build-linux/test/shots.img
run $SHELL_BIN build-linux/test/shots.img "gap:0.03 sleep:1.5 keydown:phone_power sleep:0.1 keydown:vol_down sleep:0.1 keyup:vol_down keyup:phone_power sleep:1.2 keydown:vol_down sleep:0.1 keydown:phone_power sleep:0.1 keyup:phone_power keyup:vol_down sleep:0.5"
nshots=$(mdir -b -i build-linux/test/shots.img@@8388608 ::/Pictures/Screenshots 2>/dev/null | grep -c '\.png$')
first=$(mdir -b -i build-linux/test/shots.img@@8388608 ::/Pictures/Screenshots 2>/dev/null | grep '\.png$' | head -1)
sig=$( [ -n "$first" ] && mtype -i build-linux/test/shots.img@@8388608 "$first" | head -c 8 | od -An -tx1 | tr -d ' \n')
out+=$'\n'"shots: $nshots, signature $sig"
check "Power + Volume Down saves screenshots as PNG files" \
    '^shell: screenshot Screenshot_[0-9]{8}_[0-9]{6}\.png, [0-9]+ KB$' \
    '^shell: toast "Screenshot saved"$' \
    '^shots: [12], signature 89504e470d0a1a0a$' \
    '!shell: lock \(Power\)' \
    '!shell: power menu'

# The lock screen (kernel/lockscreen.c): a tap only hints, a short swipe
# springs back, a long one (300 px up) unlocks.
path; add sleep:0.5 phone_power sleep:1.2; click; add press:1; go 360 580; add release sleep:0.3 press:1
go 360 500; go 360 400; go 360 340; add release sleep:1.5
run $SHELL_BIN build-linux/test/disk.img "$P"
check "the lock screen: swipe up to unlock" \
    '^lock: locked' \
    '^lock: swipe up to unlock$' \
    '^lock: swiped up$' \
    '^lock: unlocked$'

# Logging in (two users, so a login: the first one's, as the phone has no
# picker): a swipe up, the PIN pad; 5555 OK is wrong, 1234 OK is right.
# The pad's keys: columns 150, 360, 570; rows 582, 758, 934, 1110 (Del 0 OK).
path; add sleep:0.6 press:1; go 360 500; go 360 400; go 360 340; add release sleep:0.5
go 360 758; click; click; click; click; go 570 1110; click; add sleep:1.5
go 150 582; click; go 360 582; click; go 570 582; click; go 150 758; click; go 570 1110; click; add sleep:2
run $SHELL_BIN build-linux/test/users.img "$P"
check "the login screen: a swipe up, the PIN pad, a wrong PIN, then the right one" \
    '^users: 2 \(User admin password, Ann\)$' \
    '^lock: login screen \(2 users\)$' \
    '^lock: PIN for User$' \
    '^lock: wrong PIN for User \(1\)$' \
    '^users: User logged in$' \
    '^lock: logged in as User$' \
    '!lock: logged in as Ann'

# The VM's keyboard types the PIN too; Back (Esc) leaves the pad for the clock.
run $SHELL_BIN build-linux/test/users.img "sleep:0.6 ret sleep:0.5 esc sleep:0.3 ret sleep:0.3 1 2 3 4 ret sleep:2" \
    --keyboard qwerty
check "the PIN from the keyboard; Esc back to the clock" \
    '^lock: PIN for User$' \
    '^lock: logged in as User$'

# Holding Power on the lock screen: its power menu; Back closes it.
run $SHELL_BIN build-linux/test/users.img "sleep:0.6 keydown:phone_power sleep:0.8 keyup:phone_power sleep:0.3 phone_back sleep:0.3"
check "the lock screen's power menu: Power held, Back closes it" \
    '^lock: power menu open$' \
    '^lock: power menu closed$'

# The QWERTY layout (the build's, LiteKern Go): the keys q, 1 and m type q, 1 and m.
run $SHELL_BIN build-linux/test/disk.img "sleep:0.3 q 1 m"
check "the build's QWERTY layout" \
    '^kbd: layout qwerty$' \
    "^kbd: key 0x010 'q'$" \
    "^kbd: key 0x002 '1'$" \
    "^kbd: key 0x032 'm'$"

# The screen saver (SCREENSAVER=2): on after 2 s, a key ends it.
SCREEN="corner=" run build-linux/saver/lkx-shell build/test-saver/litekernx.img "sleep:3.5 shot:build-linux/test/saver.png a sleep:1"
out+=$'\n'$(python3 tests/lib/qemu_monitor.py check build-linux/test/saver.png corner=000000 2>&1 | sed 's/^screen:/saver-screen:/')
check "the screen saver starts after the idle time, covers the desktop, a key ends it" \
    '^screensaver: on \(idle 2 s\)$' \
    '^saver-screen: corner 000000$' \
    '^screensaver: off after [0-9]+ s: [1-9][0-9]* frames'

# --- the apps, each its own Linux process (phase L2) ------------------------------------------
open_app() { tap_app "$1"; }

path; open_app 1; add 5 shift-equal 3 ret sleep:0.5
SCREEN="has=ffffff@330,110,700,230" run $SHELL_BIN build-linux/test/disk.img "$P" --keyboard qwerty
check "Calculator runs as a Linux process: keys in, its window drawn" \
    '^desktop: open Calculator$' \
    '^user: calculator: open$' \
    '^user: calculator: 5 \+ 3 = 8$' \
    '^screen: has ffffff in 330,110,700,230: yes$' \
    '!crashed|stopped|PANIC'

# Clock (the 4th icon): its tabs along the bottom (y 1220: Alarm 120, Timer
# 360, Stopwatch 600). The timer: 5 typed (5 s), Enter starts it; it goes off.
path; open_app 3; go 360 1220; click; add sleep:0.5 5 ret sleep:6.5
run $SHELL_BIN build-linux/test/disk.img "$P" --keyboard qwerty
check "Clock: the timer counts down and goes off, with a notification" \
    '^user: clock: tab Timer$' \
    '^user: clock: timer start 00:00:05$' \
    '^user: clock: timer done$' \
    '^notify: Clock: Timer$' \
    '!crashed|stopped|PANIC'

# The stopwatch: Start (524,532), a lap (196,532), Stop, Reset.
path; open_app 3; go 600 1220; click; add sleep:0.5; go 524 532; click; add sleep:1.2; go 196 532; click
add sleep:0.3; go 524 532; click; add sleep:0.3; go 196 532; click; add sleep:0.3
run $SHELL_BIN build-linux/test/disk.img "$P"
check "Clock: the stopwatch's Start, Lap, Stop and Reset" \
    '^user: clock: stopwatch start$' \
    '^user: clock: stopwatch lap 1 00:01\.[0-9]{2}$' \
    '^user: clock: stopwatch stop$' \
    '^user: clock: stopwatch reset$'

# An alarm: + Add alarm (360,1088), the hour's + (186,336: an hour from now, so it can't ring during the test), Save (360,812):
# saved in Clock/alarms.txt. After a restart it's still there; its switch
# (604,356) turns it off.
cp build/litekernx.img build-linux/test/clock.img
path; open_app 3; go 360 1088; click; add sleep:0.5; go 186 336; click; go 360 812; click; add sleep:0.5
run $SHELL_BIN build-linux/test/clock.img "$P"
check "Clock: an alarm made and saved" \
    '^user: clock: new alarm$' \
    '^user: clock: alarm set [0-9]{2}:[0-9]{2}$' \
    '^user: clock: alarms saved \(1\): done$'
path; open_app 3; go 604 356; click; add sleep:0.5
run $SHELL_BIN build-linux/test/clock.img "$P"
check "Clock: the alarm kept across a restart; its switch turns it off" \
    '^user: clock: 1 alarm$' \
    '^user: clock: alarm [0-9]{2}:[0-9]{2} off$'

# Gallery (the 6th icon): two screenshots first (Power + Volume Down), so
# there are pictures; its grid, newest first; a tap on the first (118,266)
# opens it, a swipe left goes to the next; Delete (588,1222) asks, Enter
# deletes; Back goes back to the grid.
cp build/litekernx.img build-linux/test/gallery.img
path; add sleep:1 keydown:phone_power keydown:vol_down keyup:vol_down keyup:phone_power sleep:1.2
add keydown:phone_power keydown:vol_down keyup:vol_down keyup:phone_power sleep:1; tap_app 5; add sleep:1
go 118 266; click; add sleep:1.5 press:1; go 300 700; go 200 700; go 50 700; add release sleep:1.5
go 588 1222; click; add sleep:0.8 ret sleep:1 phone_back sleep:0.8
run $SHELL_BIN build-linux/test/gallery.img "$P"
check "Gallery: the pictures' grid, one opened, a swipe to the next, Delete" \
    '^user: gallery: 2 pictures$' \
    '^user: gallery: open Screenshot_[0-9_]+\.png$' \
    '^user: gallery: picture Screenshot_[0-9_]+\.png 720x1280 PNG' \
    '^user: gallery: delete Screenshot_[0-9_]+\.png: done$' \
    '!crashed|stopped|PANIC'

# The Task Manager (the 7th icon): Calculator opened first, then Home; it
# lists both (itself marked), with their memory; Calculator's Stop
# (597,610) asks, Enter stops it.
path; open_app 1; add phone_home sleep:0.8; tap_app 6; add sleep:1.5; go 597 610; click; add sleep:0.8 ret sleep:1.5
run $SHELL_BIN build-linux/test/disk.img "$P"
check "Task Manager: the running apps, and Stop ends one" \
    '^user: taskmgr: open, 2 apps running$' \
    '^user: taskmgr: stop Calculator: done$' \
    '^lkx: Calculator was stopped from the Task Manager$' \
    '!crashed|PANIC'

# Calculator's keys by touch: 7 (101,770), x (617,770), 1 (101,1052), Del
# (274,629), 6 (446,911), = (617,1193): 7 x 6 = 42.
path; open_app 1; go 101 770; click; go 617 770; click; go 101 1052; click; go 274 629; click
go 446 911; click; go 617 1193; click; add sleep:0.5
run $SHELL_BIN build-linux/test/disk.img "$P"
check "Calculator by touch: its keypad, Del" \
    '^user: calculator: 7 x 6 = 42$' \
    '!crashed|stopped|PANIC'

# Files lists the disk; a single tap on Documents (its first row, 300,261) opens it.
# The phone's Back goes up to the disk first (Files asked for it), then home,
# Files kept open behind.
path; open_app 0; go 300 261; click; add sleep:1 phone_back sleep:1 phone_back sleep:1.5
run $SHELL_BIN build-linux/test/disk.img "$P"
check "Files lists the disk; Back goes up a folder, then home (Files kept open)" \
    '^user: files: open Boot disk \(LITEKERNX\)$' \
    '^user: files: open folder Documents$' \
    '^shell: Back \(to Files\)$' \
    '^shell: Back leaves Files open, goes home$' \
    '^desktop: minimise Files$' \
    '!lkx: Files exited' \
    '!crashed|stopped|PANIC'

# A long press on README.TXT (its third row, 300,437): the sheet; Rename
# (its second choice, 300,1123) to README.md (three Backspaces, md, Enter). Again on
# it: Delete (300,1211), confirmed. A press held only briefly opens nothing.
cp build/litekernx.img build-linux/test/files.img
path; open_app 0; go 300 437; add press:1 sleep:0.8 release sleep:0.5; go 300 1123; click
add sleep:0.8 backspace backspace backspace m d ret sleep:1; go 300 437; add press:1 sleep:0.8 release sleep:0.5; go 300 1211; click
add sleep:0.8 ret sleep:1
run $SHELL_BIN build-linux/test/files.img "$P" --keyboard qwerty
check "Files: a long press brings up Open, Rename, Delete" \
    '^user: files: sheet README.TXT$' \
    '^user: files: rename README.TXT -> README.md$' \
    '^user: files: sheet README.md$' \
    '^user: files: delete README.md$' \
    '!crashed|stopped|PANIC'

# Pictures: a screenshot (Power + Volume Down), then Files: Pictures (300,349),
# Screenshots (300,261), the picture (300,261) shown; Set on Lock Screen (192,1222).
cp build/litekernx.img build-linux/test/pics.img
path; add sleep:1 keydown:phone_power keydown:vol_down keyup:vol_down keyup:phone_power sleep:1; open_app 0
go 300 349; click; add sleep:1; go 300 261; click; add sleep:1; click; add sleep:2.5; go 192 1222; click; add sleep:1.5
run $SHELL_BIN build-linux/test/pics.img "$P"
check "Files shows a screenshot; its button puts it on the lock screen" \
    '^user: files: open folder Screenshots$' \
    '^user: files: picture Screenshot_[0-9_]+\.png 720x1280 PNG' \
    '^user: files: lock screen set$' \
    '!crashed|stopped|PANIC'

# The on-screen keyboard: Files' New Folder (146,1229) opens a dialog with a
# text field, so it comes up and Files shrinks above it; h (435,1010), i
# (540,904), Shift then i: "hiI"; Enter (645,1222) creates it, and it goes.
cp build/litekernx.img build-linux/test/osk.img
path; open_app 0; go 146 1229; click; add sleep:1; go 435 1010; click; go 540 904; click; go 67 1116; click
go 540 904; click; add sleep:0.3; go 645 1222; click; add sleep:1
run $SHELL_BIN build-linux/test/osk.img "$P"
check "the on-screen keyboard: up for a text field, Files above it, taps type, Shift, Enter" \
    '^osk: shown$' \
    '^wm: open Files at 0,56 720x1224$' \
    '^osk: key h$' \
    '^osk: shift on$' \
    '^osk: key i \(shift\)$' \
    '^user: files: create folder hiI$' \
    '^osk: hidden$' \
    '!crashed|PANIC'

# The app switcher: Calculator, then Files; Recent apps shows both, Files
# in the middle, Calculator's card to its left (50,620): a tap goes to it.
# Again: Calculator's card (now in the middle, 360,620) swiped up closes it;
# the switcher stays, and its Close all (360,1196) closes Files.
path; open_app 1; add phone_home sleep:0.8; open_app 0; add phone_recents sleep:0.8; go 50 620; click; add sleep:1.5
add phone_recents sleep:0.8; go 360 620; add press:1; go 360 520; go 360 320; go 360 120; add release sleep:1.5
go 360 1196; click; add sleep:1.5
run $SHELL_BIN build-linux/test/disk.img "$P"
check "the app switcher: a tap goes to an app, a swipe up closes one, Close all the rest" \
    '^switcher: open, 2 apps$' \
    '^switcher: to Calculator$' \
    '^switcher: close Calculator$' \
    '^lkx: Calculator exited \(0\)$' \
    '^switcher: close all$' \
    '^lkx: Files exited \(0\)$' \
    '!crashed|PANIC'

# The switcher settles on a card when let go (One UI's way): a short drag
# springs back to Files, a long one goes on to Calculator. What's behind it,
# and behind the power menu, is blurred once as they open.
path; open_app 1; add phone_home sleep:0.8; open_app 0; add phone_recents sleep:1
go 300 620; add press:1; go 340 620; go 380 620; go 400 620; add sleep:0.2 release sleep:1
go 100 620; add press:1; go 200 620; go 300 620; go 400 620; add sleep:0.2 release sleep:1
add phone_recents sleep:1 keydown:phone_power sleep:1 keyup:phone_power sleep:0.6 phone_back sleep:1
run $SHELL_BIN build-linux/test/disk.img "$P"
check "the switcher settles on the nearest card; it and the power menu blur what's behind" \
    '^switcher: settles on Files$' \
    '^switcher: settles on Calculator$' \
    '^desktop: backdrop blurred in [0-9]+ ms$' \
    '^shell: power menu open$' \
    '^fx: [0-9]+ frames in [0-9]+ ms' \
    '!crashed|PANIC'

# Notify's two notifications (the tests' image), then Power: their cards
# under the lock screen's clock (white titles, 80..640 x 580..800).
path; tap_app 10; add phone_home sleep:0.8 phone_power sleep:1.5
cp build/test-apps/litekernx.img build-linux/test/locknote.img
SCREEN="has=ffffff@80,580,640,800" run $SHELL_BIN build-linux/test/locknote.img "$P"
check "the lock screen shows notifications under the clock" \
    '^notify: Notify: Again$' \
    '^lock: locked' \
    '^screen: has ffffff in 80,580,640,800: yes$'

# Back, in an app with nothing to go back to, goes home: the app is kept
# open behind (Android 12's way). Its icon brings it back as it was: the 7
# typed before Back is still there (7 + 6 = 13). Home keeps it open too.
path; open_app 1; add 7 phone_back sleep:1; tap_app 1; add shift-equal 6 ret sleep:0.5 phone_home sleep:1; tap_app 1
run $SHELL_BIN build-linux/test/disk.img "$P"
check "Back and Home go home, the app kept open: Calculator comes back as it was" \
    '^shell: Back leaves Calculator open, goes home$' \
    '^desktop: minimise Calculator$' \
    '^desktop: restore Calculator$' \
    '^user: calculator: 7 \+ 6 = 13$' \
    '^shell: Home$' \
    '!lkx: Calculator exited|crashed|PANIC' \
    '!shell: Back \(to'

# Settings (apps/settings): a list of pages from y 172, rows 88 (Wi-Fi 216,
# Display 304, Lock Screen 392, Battery 480, Date and Time 568, About 656);
# the header's back arrow, or Back, goes back to it. Display: the Light card
# (530,320), saved; after a restart it's still light.
cp build/litekernx.img build-linux/test/settings.img
path; open_app 2; go 360 304; click; add sleep:1; go 530 320; click; add sleep:1 phone_back sleep:0.8
go 360 656; click; add sleep:0.8 phone_back sleep:0.5 phone_back sleep:1
run $SHELL_BIN build-linux/test/settings.img "$P"
check "Settings: the list of pages, Display's style saved, About, Back to the list, then Back goes home" \
    '^user: settings: page display$' \
    '^user: settings: style light$' \
    '^settings: saved$' \
    '^user: settings: page about$' \
    '^user: settings: page home$' \
    '^shell: Back leaves Settings open, goes home$' \
    '!refused|crashed|PANIC'
run $SHELL_BIN build-linux/test/settings.img "sleep:0.5"
check "Settings' choices are kept across a restart" \
    '^settings: loaded( for User)?: light, '

# The PIN: Lock Screen (360,392), Set PIN (360,420), 1234 twice (the
# keyboard comes up for it). Then Power locks; a swipe up asks for the PIN.
cp build/litekernx.img build-linux/test/pin.img
path; open_app 2; go 360 392; click; add sleep:1; go 360 420; click; add sleep:0.8 1 2 3 4 ret sleep:0.5 1 2 3 4 ret sleep:2
add phone_power sleep:1.5 press:1; go 360 300; go 360 200; go 360 100; add release sleep:1 1 2 3 4 ret sleep:2
run $SHELL_BIN build-linux/test/pin.img "$P" --keyboard qwerty
check "Settings sets a PIN; the lock screen then asks for it" \
    '^user: settings: page lock$' \
    '^user: settings: lock PIN set$' \
    '^osk: shown$' \
    '^user: settings: lock PIN saved: done$' \
    '^lock: PIN for User$' \
    '^lock: unlocked$' \
    '!refused|crashed|PANIC'
# After a restart, it's needed to log in; then Remove PIN (530,420) takes
# the PIN once, and the next start needs none.
path; add sleep:0.6 ret sleep:0.3 1 2 3 4 ret sleep:2; open_app 2; go 360 392; click; add sleep:1; go 530 420; click
add sleep:0.8 1 2 3 4 ret sleep:2
run $SHELL_BIN build-linux/test/pin.img "$P" --keyboard qwerty
check "the PIN at the next start; Remove PIN" \
    '^lock: login screen \(1 user\)$' \
    '^lock: logged in as User$' \
    '^user: settings: lock PIN remove$' \
    '^user: settings: lock PIN removed: done$'
run $SHELL_BIN build-linux/test/pin.img "sleep:0.8"
check "no PIN: the next start goes straight in" \
    '^desktop: ready$' \
    '!lock: login screen'

# The machine (phase L4), with a battery and backlight of our own (folders
# shaped like Linux's /sys): the battery in the status bar and on the Battery
# page (360,480); Display's brightness slider (54..666 at y 247) dragged to
# 60% writes the backlight (9 of 15); Date and Time (360,568): Paris (541).
# Both are kept on the disk for the next start.
F=build-linux/test/fake
rm -rf $F; mkdir -p $F/ps/BAT0 $F/bl/acpi_video0
printf 'Battery\n' >$F/ps/BAT0/type; printf '72\n' >$F/ps/BAT0/capacity; printf 'Discharging\n' >$F/ps/BAT0/status
printf 'firmware\n' >$F/bl/acpi_video0/type; printf '15\n' >$F/bl/acpi_video0/max_brightness
printf '3\n' >$F/bl/acpi_video0/brightness
export LKX_POWER_SUPPLY=$PWD/$F/ps LKX_BACKLIGHT=$PWD/$F/bl
cp build/litekernx.img build-linux/test/machine.img
path; open_app 2; go 360 480; click; add sleep:1 phone_back sleep:0.5; go 360 304; click; add sleep:1
go 176 247; add sleep:0.2 press:1; go 300 247; go 423 247; add release sleep:1 phone_back sleep:0.5
go 360 568; click; add sleep:1; go 360 541; click; add sleep:1
SCREEN="has=ffffff@560,5,700,50" run $SHELL_BIN build-linux/test/machine.img "$P"
out+=$'\n'"backlight: $(cat $F/bl/acpi_video0/brightness)"
check "the battery in the status bar and Settings; the brightness; a time zone" \
    '^machine: backlight acpi_video0$' \
    '^screen: has ffffff in 560,5,700,50: yes$' \
    '^user: settings: page battery$' \
    '^user: settings: brightness 60: set$' \
    '^backlight: 9$' \
    '^user: settings: page time$' \
    '^machine: time zone Paris, Brussels$' \
    '!refused|crashed|PANIC'
run $SHELL_BIN build-linux/test/machine.img "sleep:0.5"
check "the time zone and brightness are kept across a restart" \
    '^machine: time zone Paris, Brussels; the clock says '
unset LKX_POWER_SUPPLY LKX_BACKLIGHT

# Blocksmith: F5 runs the starter project, a click on the stage, Esc, Ctrl+S
# saves it as "Test" (SYS_FS_WRITE).
cp build/litekernx.img build-linux/test/blocksmith.img
path; open_app 4; add sleep:0.5 f5 sleep:1; go 850 250; click; add sleep:0.5 esc sleep:0.3 ctrl-s sleep:0.5 shift-t e s t ret sleep:1
run $SHELL_BIN build-linux/test/blocksmith.img "$P" --keyboard qwerty
check "Blocksmith: runs scripts, handles stage clicks, saves projects" \
    '^user: blocksmith: run$' \
    '^user: blocksmith: print Hello from Blocksmith!$' \
    '^user: blocksmith: print You clicked at x = [0-9]+$' \
    '^user: blocksmith: saved Test\.blocks \(23 blocks\)$' \
    '!blocksmith: error|crashed'

# Stability: Files opened, sent home with Back, brought back, then closed
# from Recent apps' Close all (360,1196), 8 times (about 5 s each: run's limit
# is 50 s); each must end cleanly.
path; add gap:0.1
files() { go "$(tile_x 0)" "$(tile_y 0)"; click; add sleep:0.6; }
for i in $(seq 8); do files; add phone_back sleep:0.5; files; add phone_recents sleep:0.5; go 360 1196; click; add sleep:0.6; done
run $SHELL_BIN build-linux/test/disk.img "$P"
n=$(grep -c '^lkx: Files exited (0)$' <<<"$out")
out+=$'\n'"stress: $n of 8 ended"
check "Files opened, sent home, back and closed 8 times, ending cleanly each time" \
    '^stress: 8 of 8 ended$' \
    '!crashed|stopped|PANIC'

# Misbehaving apps (tests/apps) must only end themselves: Crash writes to
# address 0, Badcalls passes kernel pointers and nonsense (the same answers
# as the kernel's), Hang spins until the watchdog. Then Files still works.
# Their home screen icons: Crash, Badcalls, Hang are the 8th to 10th.
path
for i in 7 8 9; do add phone_home sleep:0.5; tap_app "$i"; done
add sleep:11 phone_home sleep:0.5; tap_app 0
cp build/test-apps/litekernx.img build-linux/test/apps.img
run $SHELL_BIN build-linux/test/apps.img "$P"
check "apps: a crash, bad calls and a hang each end only the app" \
    '^lkx: Crash crashed \(page fault\) and was stopped; the system carries on$' \
    '^user: badcalls: present before open -22$' \
    '^user: badcalls: unknown call -38$' \
    '^user: badcalls: list into kernel memory -14$' \
    '^user: badcalls: path in kernel memory -14$' \
    '^user: badcalls: bad volume -19$' \
    '^user: badcalls: dot-dot path -22$' \
    '^user: badcalls: missing folder -2$' \
    '^user: badcalls: bad name -22$' \
    '^user: badcalls: window into kernel memory -14$' \
    '^user: badcalls: header with 99 buttons -22$' \
    '^user: badcalls: font into kernel memory -14$' \
    '^user: badcalls: write from kernel memory -14$' \
    '^user: badcalls: write too big -14$' \
    '^user: badcalls: too much memory -22$' \
    '^user: badcalls: wallpaper from kernel memory -14$' \
    '^user: badcalls: wallpaper too big -22$' \
    '^lkx: Badcalls exited \(7\)$' \
    '^user: no response for 10 s' \
    '^lkx: Hang stopped responding and was stopped$' \
    '^user: files: open Boot disk' \
    '!still running|PANIC|Segmentation'

# Notifications and quick settings (the tests' image): Notify (the 11th icon)
# posts two; the status bar is pulled down (from 360,20 to 920): the panel,
# with them newest first. A tap on the first ("Again", 360,526) opens its app
# and dismisses it. Pulled again: Dark mode (276,236) and Do not disturb
# (444,236) tiles, then Back closes it. Home held opens it too.
path; tap_app 10; add phone_home sleep:0.8; go 360 20; add press:1; go 360 300; go 360 600; go 360 920; add release sleep:1
go 360 526; click; add sleep:1 phone_home sleep:0.8; go 360 20; click; add sleep:1; go 276 236; click; add sleep:0.5
go 444 236; click; add sleep:0.5 phone_back sleep:1 keydown:phone_home sleep:0.8 keyup:phone_home sleep:1
cp build/test-apps/litekernx.img build-linux/test/qs.img
run $SHELL_BIN build-linux/test/qs.img "$P"
check "notifications: posted, in the pulled-down panel, a tap opens the app; the panel's tiles; Back, Home held" \
    '^notify: Notify: Hello$' \
    '^notify: Notify: Again$' \
    '^qs: open$' \
    '^notify: dismissed Again$' \
    '^qs: closed$' \
    '^qs: light style$' \
    '^qs: do not disturb on$' \
    '^shell: quick settings \(Home held\)$' \
    '!crashed|PANIC'

# The kernel's own UI self-tests, unchanged, on Linux.
for t in wm gfx widgets; do
    run build-linux/selftest-$t/lkx-shell build-linux/test/disk.img "sleep:0.5"
    check "the kernel's $t self-test passes on Linux" \
        "^selftest: $t ([0-9]+)/\\1 passed$" \
        '!selftest: FAIL|PANIC|Segmentation'
done

echo
[ $failures -eq 0 ] && echo "linux: all tests passed" || { echo "linux: $failures FAILED"; exit 1; }
