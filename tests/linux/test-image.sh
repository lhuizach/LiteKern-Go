#!/usr/bin/env bash
# LiteKern X on Linux (LiteKernX-Roadmap/06, phase L3): the USB stick image,
# build-linux/litekernx-linux.img (linux/build.sh), booted in QEMU as the
# kernel version's tests boot theirs: SYSLINUX, Linux, lkx-start, the desktop
# on QEMU's screen (DRM) with its keyboard and mouse (evdev). Typing is
# by key position, and the image is AZERTY, as on the EeePC.
#
# Usage (in WSL, after linux/build.sh): bash tests/linux/test-image.sh
set -uo pipefail
cd "$(dirname "$0")/../.."

failures=0
[ -f build-linux/litekernx-linux.img ] || { echo "no build-linux/litekernx-linux.img: run linux/build.sh"; exit 1; }
mkdir -p build-linux/test-image

# boot_until IMAGE REGEX SECONDS -> $out holds the serial output (tests/kernel's)
#   KEYS="..." typed once REGEX matches, then waits for KEYS_DONE;
#   SCREEN="checks" a screenshot's checks; WAIT_OFF=1: did the VM switch off?
boot_until() {
    local img=$1 re=$2 secs=$3 log pid i sock=/tmp/lkx-image-mon-$$.sock
    local shot=build-linux/test-image/shot.png
    log=$(mktemp)
    rm -f "$sock"
    # QEMU's display offers 1024x768 first (the kernel tests' layout); the
    # EeePC's panel offers its own 1024x600.
    bash vm/qemu.sh --headless --image "$img" -- -monitor "unix:$sock,server,nowait" \
        -global VGA.xres=1024 -global VGA.yres=768 >"$log" 2>&1 &
    pid=$!
    for ((i = 0; i < secs * 10; i++)); do
        grep -Eq "$re" "$log" && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    sleep 0.5
    if [ -n "${KEYS:-}" ]; then
        python3 tests/lib/qemu_monitor.py keys "$sock" $KEYS
        for ((i = 0; i < 100; i++)); do
            grep -Eq "$KEYS_DONE" "$log" && break
            sleep 0.1
        done
        sleep 0.3
    fi
    qemu_off=0
    if [ -n "${WAIT_OFF:-}" ]; then
        for ((i = 0; i < 100; i++)); do
            kill -0 "$pid" 2>/dev/null || { qemu_off=1; break; }
            sleep 0.1
        done
    fi
    if [ -n "${SCREEN:-}" ]; then
        python3 tests/lib/qemu_monitor.py shot "$sock" "$shot" &&
            screen_out=$(python3 tests/lib/qemu_monitor.py check "$shot" $SCREEN)
    fi
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    out=$(tr -d '\r' <"$log")
    [ -n "${SCREEN:-}" ] && out+=$'\n'"${screen_out:-screen: no screenshot}"
    [ -n "${WAIT_OFF:-}" ] && out+=$'\n'"qemu: switched off $qemu_off"
    rm -f "$log" "$sock"
}

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
        sed '/^$/d; s/^/      /' <<<"$out" | tail -60
    fi
}

# QEMU's screen is 1024x768 (Bochs): the kernel tests' layout. The dock: Files,
# Log, Show Apps; the app menu one row of 6 at y 363.
path() { PX=512 PY=384 P=""; }
go() { P+=" move:$(( $1 - PX )),$(( $2 - PY ))"; PX=$1 PY=$2; }
click() { P+=" sleep:0.2 press:1 release"; }
add() { P+=" $*"; }
DOCK_Y=728 SHOWAPPS_X=572 GRID_Y=363
tile() { echo $(( (1024 - 6 * 112) / 2 + 56 + $1 * 112 )); }
ready_re='^desktop: ready'

cp build-linux/litekernx-linux.img build-linux/test-image/disk.img
SCREEN="corner=" boot_until build-linux/test-image/disk.img "$ready_re" 120
check "boots: SYSLINUX, Linux, lkx-start finds the stick, the desktop on QEMU's screen" \
    '^lkx-start: disk /dev/sda after [0-9]+ ms$' \
    '^LiteKern X \(on Linux\)$' \
    '^drm: [0-9]+x[0-9]+ on connector' \
    '^input: .*\(keyboard' \
    '^input: .*pointer' \
    '^storage: boot0 partition at 8192 \(FAT32\): FAT32, read/write$' \
    '^ramdisk: [0-9]+ files' \
    '^lkx: Calculator ' \
    '^desktop: ready$' \
    '!^screen: corner 202634|^screen: corner 000000|PANIC|Kernel panic|Segmentation'

# Calculator from the app menu, 5 + 3 typed on QEMU's keyboard (AZERTY: the
# digits are shifted; = gives +).
path; go $SHOWAPPS_X $DOCK_Y; click; add sleep:0.8; go "$(tile 1)" $GRID_Y; click
add sleep:2 shift-5 shift-equal shift-3 ret sleep:1
KEYS="$P" KEYS_DONE='^user: calculator: 5 \+ 3' boot_until build-linux/test-image/disk.img "$ready_re" 120
check "an app runs on the real thing: QEMU's mouse opens Calculator, its keyboard types into it" \
    '^desktop: open Calculator$' \
    '^user: calculator: open$' \
    '^user: calculator: 5 \+ 3 = 8$' \
    '!crashed|PANIC'

KEYS="meta_l-l" KEYS_DONE='^lock: locked' SCREEN="has=ffffff@380,180,660,330" \
    boot_until build-linux/test-image/disk.img "$ready_re" 120
check "Super+L locks: the big clock" \
    '^shell: lock \(Super\+L\)$' \
    '^lock: locked' \
    '^screen: has ffffff in 380,180,660,330: yes$'

# The power menu's Shut Down (its second item, y 74-108): Linux switches the VM off.
path; go 998 15; click; add sleep:0.5; go 930 91; click
KEYS="$P" KEYS_DONE='^power: off' WAIT_OFF=1 boot_until build-linux/test-image/disk.img "$ready_re" 120
check "Shut Down switches the machine off (Linux's power-off)" \
    '^power: off$' \
    '^qemu: switched off 1$'

echo
[ $failures -eq 0 ] && echo "image: all tests passed" || { echo "image: $failures FAILED"; exit 1; }
