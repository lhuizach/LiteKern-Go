#!/usr/bin/env bash
# LiteKern Go: back to Samsung's Android with Heimdall, from Linux (the Fedora
# VM, the phone passed through in VirtualBox's Devices > USB, in download mode).
#
#   bash restore-stock.sh unpack FIRMWARE.zip DIR   take the images out of the zip
#   bash restore-stock.sh check DIR                 what would be flashed (flashes nothing)
#   sudo bash restore-stock.sh quick DIR            BOOT and USERDATA: what Nura uses
#   sudo bash restore-stock.sh full DIR             also RECOVERY, SYSTEM, CACHE, HIDDEN
#
# Partitions come from this file's allowlist and nowhere else. Never here, on
# purpose: EFS, MODEM, MODEMST1, MODEMST2, FSG, FSC, BACKUP, the bootloaders
# (SBL1, ABOOT, RPM, QSEE, QHEE), sec, PARAM, the PIT, and --repartition.
# We never change those, so they never need restoring. CLAUDE.md's safety rules.
#
# Unverified: a restore through Heimdall on this phone has never been run.
# USERDATA is wiped by a restore.
set -euo pipefail

QUICK="BOOT:boot.img USERDATA:userdata.img.ext4"
FULL="BOOT:boot.img RECOVERY:recovery.img SYSTEM:system.img.ext4 CACHE:cache.img.ext4 HIDDEN:hidden.img.ext4 USERDATA:userdata.img.ext4"
BUILD=A500FUXXU1CSB1

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

unpack() {
    local zip=$1 dir=$2
    case $(basename "$zip") in
    *SM-A500FU_BTU_$BUILD*) ;;
    *) echo "not the SM-A500FU BTU $BUILD firmware: $zip" >&2; exit 1 ;;
    esac
    mkdir -p "$dir"
    unzip -p "$zip" "AP_${BUILD}_*" | tar -xf - -C "$dir" boot.img recovery.img system.img.ext4 userdata.img.ext4 2>/dev/null || true
    unzip -p "$zip" "CSC_BTU_*" | tar -xf - -C "$dir" cache.img.ext4 hidden.img.ext4 2>/dev/null || true
    # Named one by one: nothing else (BL, CP, the PIT) comes out of the zip.
    ls -la "$dir"
}

check() {
    local dir=$1 list=$2 bad=0
    for item in $list; do
        local part=${item%%:*} file=$dir/${item#*:}
        if [ -s "$file" ]; then
            printf '%-10s %12s  %s\n' "$part" "$(stat -c %s "$file")" "$file"
        else
            printf '%-10s MISSING      %s\n' "$part" "$file"
            bad=1
        fi
    done
    return $bad
}

flash() {
    local dir=$1 list=$2 args=()
    [ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
    check "$dir" "$list"
    heimdall detect
    for item in $list; do
        args+=("--${item%%:*}" "$dir/${item#*:}")
    done
    echo
    echo "About to run: heimdall flash ${args[*]}"
    echo "This erases Nura or LiteKern from the phone. USB must stay connected."
    read -r -p "Type RESTORE to go on: " answer
    [ "$answer" = RESTORE ] || { echo "stopped, nothing flashed"; exit 1; }
    heimdall flash "${args[@]}"
}

case ${1:-} in
unpack) [ $# -eq 3 ] || usage; unpack "$2" "$3" ;;
check)  [ $# -eq 2 ] || usage; echo "quick:"; check "$2" "$QUICK" || true; echo "full:"; check "$2" "$FULL" ;;
quick)  [ $# -eq 2 ] || usage; flash "$2" "$QUICK" ;;
full)   [ $# -eq 2 ] || usage; flash "$2" "$FULL" ;;
*)      usage ;;
esac
