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
rm -f build/test-apps/litekernx.img build/test-apps/stage1.bin     # FAT_MB isn't a make dependency
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
# 3 Blocksmith, then Log; the tests' apps come before Log).
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
path; tap_app 4; add phone_home sleep:1
SCREEN="corner=" run $SHELL_BIN build-linux/test/disk.img "$P"
check "an app opens full-screen from its home screen icon; Home goes back home" \
    '^desktop: open Log$' \
    '^wm: open Log at 0,56 720x1224$' \
    '^anim: open Log' \
    '^desktop: minimise Log$' \
    '!^screen: corner '$PLAIN

# Power held opens the power menu; its Lock (the third item, 618,151), then a key unlocks.
path; add keydown:phone_power sleep:0.8 keyup:phone_power sleep:0.3; go 618 151; click; add sleep:1.5 spc sleep:1.5
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
    '^shell: volume up$' \
    '^shell: volume down$' \
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

# Files lists the disk; a single tap on Documents (its first row, 300,261) opens it.
# The phone's Back goes up to the disk first (Files asked for it), then closes Files.
path; open_app 0; go 300 261; click; add sleep:1 phone_back sleep:1 phone_back sleep:1.5
run $SHELL_BIN build-linux/test/disk.img "$P"
check "Files lists the disk; Back goes up a folder, then closes Files" \
    '^user: files: open Boot disk \(LITEKERNX\)$' \
    '^user: files: open folder Documents$' \
    '^shell: Back \(to Files\)$' \
    '^shell: Back closes Files$' \
    '^lkx: Files exited \(0\)$' \
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

# Notify's two notifications (the tests' image), then Power: their cards
# under the lock screen's clock (white titles, 80..640 x 580..800).
path; tap_app 7; add phone_home sleep:0.8 phone_power sleep:1.5
cp build/test-apps/litekernx.img build-linux/test/locknote.img
SCREEN="has=ffffff@80,580,640,800" run $SHELL_BIN build-linux/test/locknote.img "$P"
check "the lock screen shows notifications under the clock" \
    '^notify: Notify: Again$' \
    '^lock: locked' \
    '^screen: has ffffff in 80,580,640,800: yes$'

# Back closes an app with nothing to go back to.
path; open_app 1; add phone_back sleep:1.5
run $SHELL_BIN build-linux/test/disk.img "$P"
check "Back closes Calculator" \
    '^shell: Back closes Calculator$' \
    '^lkx: Calculator exited \(0\)$' \
    '!shell: Back \(to'

# Settings (944x552 at 40,39; content from 41,86): the Light card, saved;
# after a restart it's still light. Then Users (header 196,63), Add User
# (201,548): "Ann", no password (no auth: User has none).
nwall=$(ls assets/wallpapers/*.png | grep -vc -- '-light\.png$')
row_w=$(( (nwall + 1) * 160 + nwall * 24 )); [ "$row_w" -lt 424 ] && row_w=424
light_x=$(( 41 + (942 - row_w) / 2 + 324 ))
cp build/litekernx.img build-linux/test/settings.img
path; open_app 2; go "$light_x" 194; click; add sleep:1; go 196 63; click; add sleep:0.8
go 201 548; click; add sleep:0.5 shift-a n n ret sleep:0.3 ret sleep:0.3 ret sleep:1.5
run $SHELL_BIN build-linux/test/settings.img "$P" --keyboard qwerty
check "Settings: a style change is saved; a user added (SYS_USER_CHANGE, checked by the shell)" \
    '^user: settings: style light$' \
    '^settings: saved$' \
    '^users: added Ann$' \
    '^user: settings: users add Ann: done$' \
    '!refused|crashed|PANIC'
run $SHELL_BIN build-linux/test/settings.img "sleep:0.5"
check "Settings' choices are kept across a restart" \
    '^settings: loaded( for User)?: light, ' \
    '^users: 2 \(User admin, Ann\)$'

# The machine (phase L4), with a battery and backlight of our own (folders
# shaped like Linux's /sys): the battery in the top bar; Settings' System tab
# (271,63): the wheel scrolls the time zones three rows a notch, so the first
# row (621,223) is then Paris; the brightness slider dragged to 60% writes
# the backlight (9 of 15). Both are kept on the disk for the next start.
F=build-linux/test/fake
rm -rf $F; mkdir -p $F/ps/BAT0 $F/bl/acpi_video0
printf 'Battery\n' >$F/ps/BAT0/type; printf '72\n' >$F/ps/BAT0/capacity; printf 'Discharging\n' >$F/ps/BAT0/status
printf 'firmware\n' >$F/bl/acpi_video0/type; printf '15\n' >$F/bl/acpi_video0/max_brightness
printf '3\n' >$F/bl/acpi_video0/brightness
export LKX_POWER_SUPPLY=$PWD/$F/ps LKX_BACKLIGHT=$PWD/$F/bl
cp build/litekernx.img build-linux/test/machine.img
path; open_app 2; go 271 63; click; add sleep:1; go 700 400; add wheel:1 sleep:0.5; go 621 223; click; add sleep:0.5
go 297 325; add sleep:0.2 press:1; go 317 325; go 337 325; add release sleep:1
SCREEN="has=ffffff@900,5,965,25" run $SHELL_BIN build-linux/test/machine.img "$P"
out+=$'\n'"backlight: $(cat $F/bl/acpi_video0/brightness)"
check "the battery in the top bar; Settings' System page: the wheel, a time zone, the brightness" \
    '^machine: backlight acpi_video0$' \
    '^screen: has ffffff in 900,5,965,25: yes$' \
    '^user: settings: page system$' \
    '^machine: time zone Paris, Brussels$' \
    '^user: settings: system brightness 60: set$' \
    '^backlight: 9$' \
    '!refused|crashed|PANIC'
run $SHELL_BIN build-linux/test/machine.img "sleep:0.5"
check "the time zone and brightness are kept across a restart" \
    '^machine: time zone Paris, Brussels; the clock says '
unset LKX_POWER_SUPPLY LKX_BACKLIGHT

# Blocksmith: F5 runs the starter project, a click on the stage, Esc, Ctrl+S
# saves it as "Test" (SYS_FS_WRITE).
cp build/litekernx.img build-linux/test/blocksmith.img
path; open_app 3; add sleep:0.5 f5 sleep:1; go 850 250; click; add sleep:0.5 esc sleep:0.3 ctrl-s sleep:0.5 shift-t e s t ret sleep:1
run $SHELL_BIN build-linux/test/blocksmith.img "$P" --keyboard qwerty
check "Blocksmith: runs scripts, handles stage clicks, saves projects" \
    '^user: blocksmith: run$' \
    '^user: blocksmith: print Hello from Blocksmith!$' \
    '^user: blocksmith: print You clicked at x = [0-9]+$' \
    '^user: blocksmith: saved Test\.blocks \(23 blocks\)$' \
    '!blocksmith: error|crashed'

# Stability: Files opened and closed with Back 15 times; each must end cleanly.
path; add gap:0.1
for i in $(seq 15); do tap_app 0; add phone_back sleep:0.6; done
run $SHELL_BIN build-linux/test/disk.img "$P"
n=$(grep -c '^lkx: Files exited (0)$' <<<"$out")
out+=$'\n'"stress: $n of 15 ended"
check "Files opened and closed with Back 15 times, ending cleanly each time" \
    '^stress: 15 of 15 ended$' \
    '!crashed|stopped|PANIC'

# Misbehaving apps (tests/apps) must only end themselves: Crash writes to
# address 0, Badcalls passes kernel pointers and nonsense (the same answers
# as the kernel's), Hang spins until the watchdog. Then Files still works.
# Their home screen icons: Crash, Badcalls, Hang are the 5th to 7th.
path
for i in 4 5 6; do add phone_home sleep:0.5; tap_app "$i"; done
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

# Notifications and quick settings (the tests' image): Notify (the 8th icon)
# posts two; the status bar is pulled down (from 360,20 to 920): the panel,
# with them newest first. A tap on the first ("Again", 360,526) opens its app
# and dismisses it. Pulled again: Dark mode (276,236) and Do not disturb
# (444,236) tiles, then Back closes it. Home held opens it too.
path; tap_app 7; add phone_home sleep:0.8; go 360 20; add press:1; go 360 300; go 360 600; go 360 920; add release sleep:1
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
