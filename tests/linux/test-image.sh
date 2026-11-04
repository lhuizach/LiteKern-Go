#!/usr/bin/env bash
# LiteKern Go: the VM image, build-linux/litekern-go.img (linux/build.sh),
# booted in QEMU (64-bit, KVM when WSL allows it) on the phone's 720x1280
# screen: SYSLINUX (the BIOS path; VirtualBox boots it from EFI), Linux,
# lkx-start, the shell on QEMU's screen (DRM) with its keyboard and mouse
# (evdev). The image's keyboard is QWERTY. QEMU's mouse is a relative one,
# the pointer starting in the middle of the screen; Power is F12 here.
# Each check's screenshot is kept in build-linux/test-image/NAME.png.
#
# Usage (in WSL, after linux/build.sh): bash tests/linux/test-image.sh
set -uo pipefail
cd "$(dirname "$0")/../.."

failures=0
[ -f build-linux/litekern-go.img ] || { echo "no build-linux/litekern-go.img: run linux/build.sh"; exit 1; }
mkdir -p build-linux/test-image
T=build-linux/test-image
FAT=8388608                             # the FAT32 partition's offset (the Makefile's FAT_START x 512)

export QEMU_BIN=qemu-system-x86_64 QEMU_MEM=2G QEMU_NAME="LiteKern Go (test)" QEMU_CPU=max
accel=()
if [ -w /dev/kvm ]; then
    accel=(-accel kvm)
    QEMU_CPU=host
fi

# boot_until IMAGE REGEX SECONDS -> $out holds the serial output
#   KEYS="..." typed once REGEX matches (tests/lib/qemu_monitor.py's keys),
#   then waits for KEYS_DONE; SCREEN="checks" a screenshot's checks (kept as
#   $T/$SHOT.png when SHOT is set); WAIT_OFF=1: did the VM switch off?
boot_until() {
    local img=$1 re=$2 secs=$3 log pid i sock=/tmp/lkx-image-mon-$$.sock
    local shot=$T/shot.png
    log=$(mktemp)
    rm -f "$sock" "$shot"
    bash vm/qemu.sh --headless --image "$img" -- -monitor "unix:$sock,server,nowait" \
        -global VGA.xres=720 -global VGA.yres=1280 "${accel[@]}" >"$log" 2>&1 &
    pid=$!
    for ((i = 0; i < secs * 10; i++)); do
        grep -Eq "$re" "$log" && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    sleep 0.5
    if [ -n "${KEYS:-}" ]; then
        python3 tests/lib/qemu_monitor.py keys "$sock" $KEYS
        for ((i = 0; i < ${KEYS_WAIT:-10} * 10; i++)); do      # KEYS_WAIT: seconds (10)
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
        [ -n "${SHOT:-}" ] && [ -f "$shot" ] && cp "$shot" "$T/$SHOT.png"
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

# The phone's screen, 720x1280 (tests/linux/test-linux.sh's layout): the home
# screen's apps in 4 columns of 168 x 184 px tiles from y 348. The image has
# the Web app too: Files, Calculator, Settings, Clock, Web, Blocksmith, Log.
path() { PX=360 PY=640 P=""; }
go() { P+=" move:$(( $1 - PX )),$(( $2 - PY ))"; PX=$1 PY=$2; }
click() { P+=" sleep:0.2 press:1 release"; }
add() { P+=" $*"; }
tile_x() { echo $(( (720 - 4 * 168) / 2 + 84 + ($1 % 4) * 168 )); }
tile_y() { echo $(( 348 + ($1 / 4) * 184 )); }
tap_app() { go "$(tile_x "$1")" "$(tile_y "$1")"; click; add sleep:2; }
ready_re='^desktop: ready'

cp build-linux/litekern-go.img $T/disk.img
SCREEN="corner=" SHOT=boot boot_until $T/disk.img "$ready_re" 120
check "boots: SYSLINUX, Linux, lkx-start finds the stick, the logo, the home screen on QEMU's screen" \
    '^lkx-start: disk /dev/sda after [0-9]+ ms$' \
    '^LiteKern X \(on Linux\)$' \
    '^drm: 720x1280 on connector' \
    '^input: .*\(keyboard' \
    '^input: .*pointer' \
    '^storage: boot0 partition at 16384 \(FAT32\): FAT32, read/write$' \
    '^ramdisk: [0-9]+ files' \
    '^splash: logo$' \
    '^splash: faded out$' \
    '^lkx: Clock ' \
    '^lkx: Web \(apps/browser/browser' \
    '^desktop: ready$' \
    '!^screen: corner 202634|^screen: corner 000000|PANIC|Kernel panic|Segmentation'

# Calculator from its home screen icon, 5 + 3 typed on QEMU's keyboard.
path; tap_app 1; add 5 shift-equal 3 ret sleep:1
KEYS="$P" KEYS_DONE='^user: calculator: 5 \+ 3' boot_until $T/disk.img "$ready_re" 120
check "an app runs on the real thing: QEMU's mouse opens Calculator, its keyboard types into it" \
    '^desktop: open Calculator$' \
    '^user: calculator: open$' \
    '^user: calculator: 5 \+ 3 = 8$' \
    '!crashed|PANIC'

KEYS="meta_l-l" KEYS_DONE='^lock: locked' SCREEN="has=ffffff@150,320,580,460" SHOT=lock \
    boot_until $T/disk.img "$ready_re" 120
check "Super+L locks: the big clock" \
    '^shell: lock \(Super\+L\)$' \
    '^lock: locked' \
    '^screen: has ffffff in 150,320,580,460: yes$'

# --- networking ---------------------------------------------------------------------------------
# Ethernet: QEMU's own network hands out 10.0.2.15; the status bar shows its
# icon, on the right (the bar looks once a second: a moment for it).
KEYS="sleep:2" KEYS_DONE='^never' KEYS_WAIT=2 SCREEN="has=ffffff@560,5,715,50" SHOT=ethernet \
    boot_until $T/disk.img '^net: Ethernet connected' 120
check "Ethernet: found, an address from DHCP, the icon in the status bar" \
    '^net: Ethernet eth0, ' \
    '^net: Ethernet connected, 10\.0\.2\.15$' \
    '^screen: has ffffff in 560,5,715,50: yes$'

# Wi-Fi, simulated (lkx-start's test hook, lkx.test=hwsim on the test copy's
# command line): an access point "LiteKern Test", WPA2, password "apple pie".
# Settings (its icon), Wi-Fi (the list's first row, 360,216): the network
# listed (its row, 360,316) once a scan finds it; a tap on it asks for the password: a wrong one
# is refused and not kept, a tap asks again, the right one joins, gets an
# address, and is remembered on the stick.
img=$T/wifi.img
cp build-linux/litekern-go.img $img
mtype -i $img@@$FAT ::/syslinux/syslinux.cfg | sed 's/^\(    APPEND .*\)$/\1 lkx.test=hwsim/' > $T/syslinux.cfg
mcopy -o -i $img@@$FAT $T/syslinux.cfg ::/syslinux/syslinux.cfg
path; tap_app 2; go 360 216; click; add sleep:10; go 360 316; click; add sleep:1.5     # 10 s: the scan finds it
add w r o n g w r o n g ret sleep:12; click; add sleep:1.5 a p p l e spc p i e ret
KEYS="$P" KEYS_DONE='^net: Wi-Fi connected to LiteKern Test' KEYS_WAIT=30 SCREEN="corner=" SHOT=wifi \
    boot_until $img "$ready_re" 120
check "Wi-Fi: Settings lists it, a wrong password is refused, the right one joins" \
    '^lkx-start: test Wi-Fi: an access point on wlan1$' \
    '^net: Ethernet eth0, Wi-Fi wlan0 \(0 remembered\)$' \
    '^user: settings: page wifi$' \
    '^net: Wi-Fi: the password for LiteKern Test was refused$' \
    '^net: Wi-Fi: remembered LiteKern Test$' \
    '^net: Wi-Fi connected to LiteKern Test, 192\.168\.77\.[0-9]+$' \
    '!PANIC|crashed'

# ...and after a restart it joins by itself.
boot_until $img '^net: Wi-Fi connected to LiteKern Test' 120
check "Wi-Fi: a remembered network is joined by itself after a restart" \
    '^net: Ethernet eth0, Wi-Fi wlan0 \(1 remembered\)$' \
    '^net: Wi-Fi connected to LiteKern Test, 192\.168\.77\.[0-9]+$'

# --- the machine and scrolling ------------------------------------------------------------------
# Linux's messages in the Log; London's time. Settings, Date and Time (its
# row, 360,568): QEMU's wheel (a notch down: mouse_move's dz -1) reaches the
# time zones through Linux (evdev) and scrolls a row, so Paris is the third
# (360,453).
path; tap_app 2; go 360 568; click; add sleep:1.5; go 360 700; add sleep:0.3 move:0,0,-1 sleep:0.8
go 360 453; click; add sleep:1
KEYS="$P" KEYS_DONE='^machine: time zone Paris' boot_until $T/disk.img "$ready_re" 120
check "the Log has Linux's messages; QEMU's wheel scrolls the time zones; one is picked" \
    "^linux: \(Linux's warnings while starting; its news from now on\)$" \
    '^machine: time zone London; the clock says ' \
    '^user: settings: page time$' \
    '^machine: time zone Paris, Brussels$' \
    '!only [0-9]+MB will be used'

# --- the browser ----------------------------------------------------------------------------------
# The Web app (NetSurf, from the system image: its own kerns.json), the fifth
# icon; it opens full-screen on the start page (dark: #222226).
path; tap_app 4; add sleep:7
KEYS="$P" KEYS_DONE='^never' KEYS_WAIT=8 SCREEN="has=222226@100,300,620,900" SHOT=web \
    boot_until $T/disk.img "$ready_re" 120
check "the Web app: listed, opens NetSurf full-screen on the start page" \
    '^desktop: open Web$' \
    '^user: browser: open$' \
    '^screen: has 222226 in 100,300,620,900: yes$' \
    '!lkx: Web (crashed|exited)|unrecognized option'

# A search over HTTPS (DuckDuckGo's HTML page: no JavaScript needed), typed
# into the start page's box (230,318). Needs the internet: skipped without it.
if curl -s -m 5 -o /dev/null https://html.duckduckgo.com/html/; then
    path; tap_app 4; add sleep:7; go 230 318; click; add sleep:0.5 e e e p c ret sleep:25
    KEYS="$P" KEYS_DONE='^never' KEYS_WAIT=34 SCREEN="has=ffffff@100,300,620,900" SHOT=web-search \
        boot_until $T/disk.img "$ready_re" 120
    check "the Web app: an HTTPS search, typed on the keyboard, shows its results" \
        '^user: browser: open$' \
        '^screen: has ffffff in 100,300,620,900: yes$' \
        '!lkx: Web (crashed|exited)'
else
    echo "SKIP  the Web app's HTTPS search (no internet here)"
fi

# A download (download_lkx.c): a file served from this machine (QEMU's
# 10.0.2.2), its address typed into the address bar (a tap on its empty right
# end, 600,162, puts the cursor after the start page's address). It lands in
# Downloads on the stick, whole.
mkdir -p $T/www
head -c 300000 /dev/urandom > $T/www/t.bin
python3 -m http.server 8123 --bind 127.0.0.1 --directory $T/www >/dev/null 2>&1 &
www=$!
sleep 0.5
cp build-linux/litekern-go.img $T/download.img
path; tap_app 4; add sleep:10; go 600 162; click; add sleep:0.3; for i in $(seq 45); do add backspace; done
add h t t p shift-semicolon slash slash 1 0 dot 0 dot 2 dot 2 shift-semicolon 8 1 2 3 slash t dot b i n ret sleep:6
KEYS="$P" KEYS_DONE='^never' KEYS_WAIT=14 SCREEN="corner=" SHOT=download boot_until $T/download.img "$ready_re" 120
kill $www 2>/dev/null
mcopy -o -i $T/download.img@@$FAT ::/Downloads/t.bin $T/got.bin 2>/dev/null
out+=$'\n'"download: $(cmp -s $T/www/t.bin $T/got.bin && echo same || echo missing)"
rm -f $T/got.bin
check "the Web app downloads a file into Downloads on the stick" \
    '^user: browser: open$' \
    '^download: same$' \
    '!lkx: Web (crashed|exited)'

# Power held (F12 here) brings up the power menu; Power off (its first
# button, 360,512): Linux switches the VM off.
path; add hold:f12:900 sleep:0.5; go 360 512; click
KEYS="$P" KEYS_DONE='^power: off' WAIT_OFF=1 boot_until $T/disk.img "$ready_re" 120
check "Power held, then Power off: the machine switches off (Linux's power-off)" \
    '^shell: power menu \(Power held\)$' \
    '^power: off$' \
    '^qemu: switched off 1$'

echo
[ $failures -eq 0 ] && echo "image: all tests passed" || { echo "image: $failures FAILED"; exit 1; }
