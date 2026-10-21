#!/usr/bin/env bash
# LiteKern X on Linux (LiteKernX-Roadmap/06, phases L1 and L2): lkx-shell, the
# desktop as a Linux program, and the apps as Linux processes, run headless with scripted input (platform/headless.c,
# the same key language as tests/kernel) at the EeePC's 1024x600. Checks its
# log, like tests/kernel/test-kernel.sh, and its screenshots.
#
# Usage (in WSL): bash tests/linux/test-linux.sh
set -uo pipefail
cd "$(dirname "$0")/../.."

failures=0
W=1024 H=600
# The shell and the apps, the tests' misbehaving ones too; their disk image
# (the kernel build's, with them in the ramdisk).
TEST_APPS="tests/apps/crash tests/apps/badcalls tests/apps/hang"
make -s -f platform/Makefile EXTRA_APPS="$TEST_APPS" >/dev/null || exit 1
rm -f build/test-apps/litekernx.img build/test-apps/stage1.bin     # FAT_MB isn't a make dependency
make -s BUILD=build/test-apps FAT_MB=64 "EXTRA_APPS=$TEST_APPS" build/test-apps/litekernx.img >/dev/null || exit 1
for t in wm gfx widgets; do
    make -s -f platform/Makefile SELFTEST=$t LBUILD=build-linux/selftest-$t >/dev/null || exit 1
done
make -s -f platform/Makefile KBUILD=build/test-saver LBUILD=build-linux/saver SCREENSAVER=2 >/dev/null || exit 1

mkdir -p build-linux/test
cp build/litekernx.img build-linux/test/disk.img

# Two users on a copy: User (an admin, password "apple") and Ann (no
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
    f"user admin 3584e4 {salt.hex()} {h(salt, b'apple')} User\n"
    f"user standard e45c84 {bytes(16).hex()} - Ann\n")
EOF
mmd -i build-linux/test/users.img@@4194304 ::/LiteKern 2>/dev/null
mcopy -o -i build-linux/test/users.img@@4194304 build-linux/test/users.txt ::/LiteKern/users.txt || exit 1

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
# The dock at 1024x600: Files, Log, Show Apps (kernel/desktop.c's layout).
DOCK_Y=$((H - 40)) FILES_X=452 LOG_X=512 SHOWAPPS_X=572
PLAIN=202634

SHELL_BIN=build-linux/lkx-shell

SCREEN="corner=" run $SHELL_BIN build-linux/test/disk.img "sleep:1"
check "starts like the kernel: the disk image's ramdisk and FAT32, the apps, the wallpaper" \
    '^LiteKern X \(on Linux\)$' \
    '^dev boot0 driver=linux-disk bound$' \
    '^storage: boot0 partition at 8192 \(FAT32\): FAT32, read/write$' \
    '^ramdisk: [0-9]+ files' \
    '^lkx: Files \(apps/files/files.lkx' \
    '^lkx: Blocksmith \(apps/blocksmith/blocksmith.lkx' \
    '^wallpaper: crossing, 1024x600' \
    '^dock: Files Log Show Apps$' \
    '^desktop: ready$' \
    '^screen: 1024x600$' \
    '!^screen: corner '$PLAIN \
    '!PANIC|Segmentation'

path; go $SHOWAPPS_X $DOCK_Y; click; add sleep:0.8 esc sleep:0.5
run $SHELL_BIN build-linux/test/disk.img "$P"
check "the app menu opens from the dock, Esc closes it" \
    '^desktop: app menu open$' \
    '^desktop: app menu closed$'

# Log from the dock (it opens at 132,75, 760x480: its close button 868,99).
path; go $LOG_X $DOCK_Y; click; add sleep:1.2; go 868 99; click; add sleep:1
SCREEN="corner=" run $SHELL_BIN build-linux/test/disk.img "$P"
check "Log opens from the dock and its close button closes it" \
    '^desktop: open Log$' \
    '^wm: open Log at 132,75 760x480$' \
    '^anim: open Log' \
    '^wm: close$' \
    '!^screen: corner '$PLAIN

# The power menu's Lock (its third item, y 108-142), then a key unlocks.
path; go 998 15; click; add sleep:0.4; go 930 125; click; add sleep:1.5 spc sleep:1.5
run $SHELL_BIN build-linux/test/disk.img "$P"
check "the power menu locks; a key unlocks" \
    '^shell: lock$' \
    '^anim: lock, ' \
    '^lock: locked \(wallpaper desktop\)$' \
    '^lock: unlocked$'

run $SHELL_BIN build-linux/test/disk.img "sleep:0.5 meta_l-l sleep:1.5 shot:build-linux/test/lock.png"
SCREEN="has=ffffff@380,140,660,250" run $SHELL_BIN build-linux/test/disk.img "sleep:0.5 meta_l-l sleep:1.5"
check "Super+L locks: the big clock" \
    '^shell: lock \(Super\+L\)$' \
    '^screen: has ffffff in 380,140,660,250: yes$'

# Two users: the login screen; User (first, Enter), a wrong password, the right one.
run $SHELL_BIN build-linux/test/users.img "sleep:0.6 ret sleep:0.5 p e a r ret sleep:1.5 a p p l e ret sleep:2" \
    --keyboard qwerty
check "the login screen: a wrong password, then the right one" \
    '^users: 2 \(User admin password, Ann\)$' \
    '^lock: login screen \(2 users\)$' \
    '^lock: password for User$' \
    '^lock: wrong password for User \(1\)$' \
    '^users: User logged in$' \
    '^lock: logged in as User$' \
    '!lock: logged in as Ann'

run $SHELL_BIN build-linux/test/users.img "sleep:0.6 right sleep:0.3 ret sleep:2" --keyboard qwerty
check "someone without a password logs straight in, with settings of their own to come" \
    '^lock: logged in as Ann$' \
    '^settings: Ann has none of their own yet$' \
    '!lock: password for Ann'

# The AZERTY layout (the build's): the keys QEMU calls q, 1 and m type a, & and ,.
run $SHELL_BIN build-linux/test/disk.img "sleep:0.3 q 1 m"
check "the build's AZERTY layout" \
    '^kbd: layout azerty$' \
    "^kbd: key 0x010 'a'$" \
    "^kbd: key 0x002 '&'$" \
    "^kbd: key 0x032 ','$"

# The screen saver (SCREENSAVER=2): on after 2 s, a key ends it.
SCREEN="corner=" run build-linux/saver/lkx-shell build/test-saver/litekernx.img "sleep:3.5 shot:build-linux/test/saver.png a sleep:1"
out+=$'\n'$(python3 tests/lib/qemu_monitor.py check build-linux/test/saver.png corner=000000 2>&1 | sed 's/^screen:/saver-screen:/')
check "the screen saver starts after the idle time, covers the desktop, a key ends it" \
    '^screensaver: on \(idle 2 s\)$' \
    '^saver-screen: corner 000000$' \
    '^screensaver: off after [0-9]+ s: [1-9][0-9]* frames'

# --- the apps, each its own Linux process (phase L2) ------------------------------------------
# The app menu at 1024x600, one row of 6: Files, Calculator, Settings, Probe,
# Blocksmith, Log (kernel/desktop.c's grid), at y 265.
tile() { echo $(( (W - 6 * 112) / 2 + 56 + $1 * 112 )); }
GRID_Y=265
open_app() { go $SHOWAPPS_X $DOCK_Y; click; add sleep:0.6; go "$(tile "$1")" $GRID_Y; click; add sleep:1.5; }

path; open_app 1; add 5 shift-equal 3 ret sleep:0.5
SCREEN="has=ffffff@330,110,700,230" run $SHELL_BIN build-linux/test/disk.img "$P" --keyboard qwerty
check "Calculator runs as a Linux process: keys in, its window drawn" \
    '^desktop: open Calculator$' \
    '^user: calculator: open$' \
    '^user: calculator: 5 \+ 3 = 8$' \
    '^screen: has ffffff in 330,110,700,230: yes$' \
    '!crashed|stopped|PANIC'

# Files from the dock; its close button (760x500 at 132,65: 868,89) ends it.
path; go $FILES_X $DOCK_Y; click; add sleep:1.5; go 868 89; click; add sleep:1
run $SHELL_BIN build-linux/test/disk.img "$P"
check "Files lists the disk (its calls through kernel/sys_app.c); closing its window ends it" \
    '^user: files: open Boot disk \(LITEKERNX\)$' \
    '^lkx: Files exited \(0\)$'

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

# A maximised window's header joins the top bar (the user's option A): Settings
# maximised (its button at 924,63) shows its tabs there; Users (275,15) in the
# top bar switches page; restore (924,15) brings its own header bar back.
path; open_app 2; go 924 63; click; add sleep:1.5; go 275 15; click; add sleep:1
go 600 300; add sleep:0.3 shot:build-linux/test/merged.png; go 924 15; click; add sleep:1
run $SHELL_BIN build-linux/test/disk.img "$P"
out+=$'\n'$(python3 tests/lib/qemu_monitor.py check build-linux/test/merged.png "has=444448@240,4,310,26" 2>&1)
check "a maximised window's tabs and buttons are in the top bar, and work there" \
    '^desktop: maximise Settings$' \
    '^user: settings: page users$' \
    '^screen: has 444448 in 240,4,310,26: yes$' \
    '^desktop: unmaximise Settings$'

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
path; open_app 4; add sleep:0.5 f5 sleep:1; go 850 250; click; add sleep:0.5 esc sleep:0.3 ctrl-s sleep:0.5 shift-t e s t ret sleep:1
run $SHELL_BIN build-linux/test/blocksmith.img "$P" --keyboard qwerty
check "Blocksmith: runs scripts, handles stage clicks, saves projects" \
    '^user: blocksmith: run$' \
    '^user: blocksmith: print Hello from Blocksmith!$' \
    '^user: blocksmith: print You clicked at x = [0-9]+$' \
    '^user: blocksmith: saved Test\.blocks \(23 blocks\)$' \
    '!blocksmith: error|crashed'

path; open_app 3; add sleep:1
run $SHELL_BIN build-linux/test/disk.img "$P"
check "the Hardware Probe reads Linux's /proc and /sys" \
    '^user: probe: open$' \
    '^user: probe: [1-9][0-9]* items$' \
    '!crashed'

# Stability: Files opened and closed 15 times; each must end cleanly.
path; add gap:0.1
for i in $(seq 15); do go $FILES_X $DOCK_Y; click; add sleep:0.8; go 868 89; click; add sleep:0.6; done
run $SHELL_BIN build-linux/test/disk.img "$P"
n=$(grep -c '^lkx: Files exited (0)$' <<<"$out")
out+=$'\n'"stress: $n of 15 ended"
check "Files opened and closed 15 times, ending cleanly each time" \
    '^stress: 15 of 15 ended$' \
    '!crashed|stopped|PANIC'

# Misbehaving apps (tests/apps) must only end themselves: Crash writes to
# address 0, Badcalls passes kernel pointers and nonsense (the same answers
# as the kernel's), Hang spins until the watchdog. Then Files still works.
# Their menu: 9 tiles (8 a row, y 213): Crash, Badcalls, Hang at 679, 791, 903.
path
for x in 679 791 903; do go $SHOWAPPS_X $DOCK_Y; click; add sleep:0.6; go $x 213; click; add sleep:1.5; done
add sleep:11; go $FILES_X $DOCK_Y; click; add sleep:1.5
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

# The kernel's own UI self-tests, unchanged, on Linux.
for t in wm gfx widgets; do
    run build-linux/selftest-$t/lkx-shell build-linux/test/disk.img "sleep:0.5"
    check "the kernel's $t self-test passes on Linux" \
        "^selftest: $t ([0-9]+)/\\1 passed$" \
        '!selftest: FAIL|PANIC|Segmentation'
done

echo
[ $failures -eq 0 ] && echo "linux: all tests passed" || { echo "linux: $failures FAILED"; exit 1; }
