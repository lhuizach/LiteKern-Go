#!/usr/bin/env bash
# LiteKern X on Linux (LiteKernX-Roadmap/06, phase L1): lkx-shell, the desktop
# as a Linux program, run headless with scripted input (platform/headless.c,
# the same key language as tests/kernel) at the EeePC's 1024x600. Checks its
# log, like tests/kernel/test-kernel.sh, and its screenshots.
#
# Usage (in WSL): bash tests/linux/test-linux.sh
set -uo pipefail
cd "$(dirname "$0")/../.."

failures=0
W=1024 H=600
make -s -f platform/Makefile >/dev/null || exit 1
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

# The kernel's own UI self-tests, unchanged, on Linux.
for t in wm gfx widgets; do
    run build-linux/selftest-$t/lkx-shell build-linux/test/disk.img "sleep:0.5"
    check "the kernel's $t self-test passes on Linux" \
        "^selftest: $t ([0-9]+)/\\1 passed$" \
        '!selftest: FAIL|PANIC|Segmentation'
done

echo
[ $failures -eq 0 ] && echo "linux: all tests passed" || { echo "linux: $failures FAILED"; exit 1; }
