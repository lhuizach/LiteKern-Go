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
# Log, Show Apps; the app menu one row of 7 at y 363 (the Web app is the image's own).
path() { PX=512 PY=384 P=""; }
go() { P+=" move:$(( $1 - PX )),$(( $2 - PY ))"; PX=$1 PY=$2; }
click() { P+=" sleep:0.2 press:1 release"; }
add() { P+=" $*"; }
DOCK_Y=728 SHOWAPPS_X=572 GRID_Y=363
tile() { echo $(( (1024 - 7 * 112) / 2 + 56 + $1 * 112 )); }   # 7: Files, Calculator, Settings, Probe, Web, Blocksmith, Log
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

# --- networking (phase L5) ---------------------------------------------------------------------
# Ethernet: QEMU's own network hands out 10.0.2.15; the top bar shows the
# Ethernet icon.
# (the top bar looks once a second: a moment for it)
KEYS="sleep:2" KEYS_DONE='^never' KEYS_WAIT=2 SCREEN="has=ffffff@935,5,975,25"     boot_until build-linux/test-image/disk.img '^net: Ethernet connected' 120
check "Ethernet: found, an address from DHCP, the icon in the top bar" \
    '^net: Ethernet eth0, ' \
    '^net: Ethernet connected, 10\.0\.2\.15$' \
    '^screen: has ffffff in 935,5,975,25: yes$'

# Wi-Fi, simulated (lkx-start's test hook, lkx.test=hwsim on the test copy's
# command line): an access point "LiteKern Test", WPA2, password "apple pie".
# Settings -> Network (its tab at 284,147): the network listed (row y 309);
# Connect (199,632) asks for the password: a wrong one is refused and not
# kept, Connect asks again, the right one joins, gets an address, and is
# remembered on the stick. AZERTY, by key position: "apple pie" is q p p l e.
img=build-linux/test-image/wifi.img
cp build-linux/litekernx-linux.img $img
mtype -i $img@@4194304 ::/syslinux/syslinux.cfg | sed 's/^\(    APPEND .*\)$/\1 lkx.test=hwsim/' \
    > build-linux/test-image/syslinux.cfg
mcopy -o -i $img@@4194304 build-linux/test-image/syslinux.cfg ::/syslinux/syslinux.cfg
SETTINGS_TILE=$(tile 2)
path; go $SHOWAPPS_X $DOCK_Y; click; add sleep:0.8; go "$SETTINGS_TILE" $GRID_Y; click; add sleep:2
go 284 147; click; add sleep:4; go 400 309; click; add sleep:0.5
go 199 632; click; add sleep:1 w r o n g w r o n g ret sleep:12
go 199 632; click; add sleep:1 q p p l e spc p i e ret
KEYS="$P" KEYS_DONE='^net: Wi-Fi connected to LiteKern Test' KEYS_WAIT=30 SCREEN="corner=" \
    boot_until $img "$ready_re" 120
cp build-linux/test-image/shot.png build-linux/test-image/network.png
check "Wi-Fi: Settings -> Network lists it, a wrong password is refused, the right one joins" \
    '^lkx-start: test Wi-Fi: an access point on wlan1$' \
    '^net: Ethernet eth0, Wi-Fi wlan0 \(0 remembered\)$' \
    '^user: settings: page network$' \
    '^net: Wi-Fi: the password for LiteKern Test was refused$' \
    '^net: Wi-Fi: remembered LiteKern Test$' \
    '^net: Wi-Fi connected to LiteKern Test, 192\.168\.77\.[0-9]+$' \
    '!PANIC|crashed'

# ...and after a restart it joins by itself.
boot_until $img '^net: Wi-Fi connected to LiteKern Test' 120
check "Wi-Fi: a remembered network is joined by itself after a restart" \
    '^net: Ethernet eth0, Wi-Fi wlan0 \(1 remembered\)$' \
    '^net: Wi-Fi connected to LiteKern Test, 192\.168\.77\.[0-9]+$'

# --- the machine and scrolling (phase L4) -------------------------------------------------------
# Linux's messages in the Log; London's time. Settings -> System (its tab at
# 363,147): QEMU's wheel (a notch down: mouse_move's dz -1) reaches the time
# zones through Linux (evdev) and scrolls three rows, so the first row
# (700,307) is Paris.
path; go $SHOWAPPS_X $DOCK_Y; click; add sleep:1; go "$SETTINGS_TILE" $GRID_Y; click; add sleep:2
go 363 147; click; add sleep:1.5; go 700 400; add sleep:0.3 move:0,0,-1 sleep:0.8; go 700 307; click; add sleep:1
KEYS="$P" KEYS_DONE='^machine: time zone Paris' boot_until build-linux/test-image/disk.img "$ready_re" 120
check "the Log has Linux's messages; QEMU's wheel scrolls Settings' time zones; one is picked" \
    "^linux: \(Linux's warnings while starting; its news from now on\)$" \
    '^machine: time zone London; the clock says ' \
    '^user: settings: page system$' \
    '^machine: time zone Paris, Brussels$' \
    '!only [0-9]+MB will be used'

# --- the browser (phase L6) ---------------------------------------------------------------------
# The Web app (NetSurf, from the system image: its own kerns.json) in the app
# menu (7 tiles at y 363: Web the fifth, x 623); it opens a LiteKern X window
# on the start page (dark: #222226); its close button ends it.
WEB_X=623
path; go $SHOWAPPS_X $DOCK_Y; click; add sleep:1; go $WEB_X $GRID_Y; click; add sleep:8
KEYS="$P" KEYS_DONE='^never' KEYS_WAIT=9 SCREEN="has=222226@100,250,600,400" \
    boot_until build-linux/test-image/disk.img "$ready_re" 120
check "the Web app: listed, opens NetSurf in a LiteKern X window on the start page" \
    '^lkx: Web \(apps/browser/browser' \
    '^desktop: open Web$' \
    '^user: browser: open$' \
    '^screen: has 222226 in 100,250,600,400: yes$' \
    '!lkx: Web (crashed|exited)|unrecognized option'

# A search over HTTPS (DuckDuckGo's HTML page: no JavaScript needed), typed
# on QEMU's keyboard. Needs the internet: skipped without it.
if curl -s -m 5 -o /dev/null https://html.duckduckgo.com/html/; then
    path; go $SHOWAPPS_X $DOCK_Y; click; add sleep:1; go $WEB_X $GRID_Y; click; add sleep:7
    go 300 335; click; add sleep:0.5 e e e p c ret sleep:25
    KEYS="$P" KEYS_DONE='^never' KEYS_WAIT=34 SCREEN="has=ffffff@100,250,600,400" \
        boot_until build-linux/test-image/disk.img "$ready_re" 120
    cp build-linux/test-image/shot.png build-linux/test-image/web-search.png
    check "the Web app: an HTTPS search, typed on the keyboard, shows its results" \
        '^user: browser: open$' \
        '^screen: has ffffff in 100,250,600,400: yes$' \
        '!lkx: Web (crashed|exited)'
else
    echo "SKIP  the Web app's HTTPS search (no internet here)"
fi

# A download (download_lkx.c): a file served from this machine (QEMU's 10.0.2.2),
# its address typed into the Web app's address bar (URL_X, URL_Y; AZERTY by key
# position: ":" is the dot key, "/" Shift+dot, "." Shift+comma, digits shifted).
# It lands in Downloads on the stick, whole.
mkdir -p build-linux/test-image/www
head -c 300000 /dev/urandom > build-linux/test-image/www/t.bin
python3 -m http.server 8123 --bind 127.0.0.1 --directory build-linux/test-image/www >/dev/null 2>&1 &
www=$!
sleep 0.5
URL_X=500 URL_Y=180
cp build-linux/litekernx-linux.img build-linux/test-image/download.img
path; go $SHOWAPPS_X $DOCK_Y; click; add sleep:1; go $WEB_X $GRID_Y; click; add sleep:7
go $URL_X $URL_Y; click; add sleep:0.3 end; for i in $(seq 45); do add backspace; done
add h t t p dot shift-dot shift-dot shift-1 shift-0 shift-comma shift-0 shift-comma shift-2 shift-comma shift-2 \
    dot shift-8 shift-1 shift-2 shift-3 shift-dot t shift-comma b i n ret sleep:6
KEYS="$P" KEYS_DONE='^never' KEYS_WAIT=14 SCREEN="corner=" boot_until build-linux/test-image/download.img "$ready_re" 120
cp build-linux/test-image/shot.png build-linux/test-image/download.png
kill $www 2>/dev/null
mcopy -o -i build-linux/test-image/download.img@@4194304 ::/Downloads/t.bin build-linux/test-image/got.bin 2>/dev/null
out+=$'\n'"download: $(cmp -s build-linux/test-image/www/t.bin build-linux/test-image/got.bin && echo same || echo missing)"
rm -f build-linux/test-image/got.bin
check "the Web app downloads a file into Downloads on the stick" \
    '^user: browser: open$' \
    '^download: same$' \
    '!lkx: Web (crashed|exited)'

# The power menu's Shut Down (its second item, y 74-108): Linux switches the VM off.
path; go 998 15; click; add sleep:0.5; go 930 91; click
KEYS="$P" KEYS_DONE='^power: off' WAIT_OFF=1 boot_until build-linux/test-image/disk.img "$ready_re" 120
check "Shut Down switches the machine off (Linux's power-off)" \
    '^power: off$' \
    '^qemu: switched off 1$'

echo
[ $failures -eq 0 ] && echo "image: all tests passed" || { echo "image: $failures FAILED"; exit 1; }
