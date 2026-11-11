#!/usr/bin/env bash
# LiteKern Go: starts a new app.
#
#   bash tools/new-app.sh NAME ["Name on the screen"]
#
# NAME: lowercase letters, digits and dashes. Makes apps/NAME/ from tools/app-template/ (a small
# working app: a window, a counter, a button, saving to a file) with its kerns.json, builds it as a
# check, and says what to do next. The app appears on the home screen by itself: nothing else to
# register, and the tests find apps by name, so adding one moves nothing.
set -euo pipefail
cd "$(dirname "$0")/.."
name=${1:-}
title=${2:-}
[[ "$name" =~ ^[a-z][a-z0-9-]*$ ]] || { sed -n '3,6p' "$0"; echo "NAME must be lowercase letters, digits and dashes, starting with a letter" >&2; exit 2; }
[ ! -e "apps/$name" ] || { echo "apps/$name is already there" >&2; exit 1; }
if [ -z "$title" ]; then
    title=$(printf '%s' "$name" | sed 's/-/ /g; s/\b\(.\)/\u\1/g')
fi
# the next place in the app order: after every app there is (the tests' apps have none: 100)
order=$(python3 - <<'PY'
import glob, json
best = 99
for f in glob.glob("apps/*/kerns.json"):
    best = max(best, int(json.load(open(f, encoding="utf-8")).get("order", 100)))
print(best + 1)
PY
)
mkdir "apps/$name"
sed "s/@NAME@/$name/g; s/@TITLE@/$title/g" tools/app-template/app.c > "apps/$name/$name.c"
sed "s/@NAME@/$name/g; s/@TITLE@/$title/g; s/@ORDER@/$order/g" tools/app-template/kerns.json > "apps/$name/kerns.json"
echo "made apps/$name/: $name.c and kerns.json (\"$title\", order $order)"
echo
echo "Next:"
echo "  edit apps/$name/$name.c               (docs/NEW-APP.md, docs/APPS.md, kernel/kern86_abi.h)"
echo "  bash tools/check-app.sh $name         builds it and opens it on the PC desktop, headless, as a check"
echo "  wsl make -f platform/Makefile run     the whole phone desktop in a window on the PC (mouse = touch)"
echo "  bash tools/push-app.sh $name          builds it for the phone and puts it there (until the next restart)"
