#!/usr/bin/env bash
# LiteKern Go, phase 6: copies the phone's recipe from pmaports (Nura's, formerly
# postmarketOS's) into phone/, so we keep it. samsung-a5 is in pmaports'
# archived/ folder (no maintainer) and could go. Run in WSL; needs no root.
set -euo pipefail
cd "$(dirname "$0")"
W=$HOME/lkx-phone
mkdir -p "$W"
[ -d "$W/pmaports" ] || git clone -q --depth 1 https://gitlab.postmarketos.org/postmarketOS/pmaports.git "$W/pmaports"
[ -d "$W/pmbootstrap" ] || git clone -q --depth 1 https://gitlab.postmarketos.org/postmarketOS/pmbootstrap.git "$W/pmbootstrap"
P=$W/pmaports/device
cp "$P"/archived/device-samsung-a5/{APKBUILD,deviceinfo,kernel-cmdline.conf,modules-initfs} device-samsung-a5/
K=$P/testing/linux-postmarketos-qcom-msm8916
cp "$K"/APKBUILD kernel/
cp "$K"/config-postmarketos-qcom-msm8916.aarch64 kernel/
cp "$K"/*.patch kernel/
git -C "$W/pmaports" log -1 --format='pmaports %H (%cd)' --date=short > PMAPORTS-COMMIT
cat PMAPORTS-COMMIT
