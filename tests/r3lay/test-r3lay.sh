#!/usr/bin/env bash
# R3lay against tests/r3lay/mock_supabase.py: no network, no real data. In WSL:
#   bash tests/r3lay/test-r3lay.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
make -s -f platform/Makefile || exit 1

O=build-linux/r3-test PORT=18742 failures=0
rm -rf "$O"
mkdir -p "$O"
cp build/litekernx.img "$O/disk.img"

mv() { P+=" move:$(($1 - PX)),$(($2 - PY))"; PX=$1 PY=$2; }
tap() { mv "$1" "$2"; P+=" sleep:0.2 press:1 release sleep:0.5"; }
typ() { for c in $(echo "$1" | sed 's/./& /g'); do P+=" $c"; done; }
begin() { PX=360 PY=640 P="sleep:1"; tap 108 608; P+=" sleep:1"; }    # opens R3lay
login() { typ abc; P+=" shift-2"; typ bcd; P+=" tab"; typ "$1"; P+=" ret sleep:2.5"; }

# run KEEP_NOTHING: the mock, then the steps in $P, against the same disk each time.
run() {
    MOCK_LOSE_REPLY=${MOCK_LOSE_REPLY:-} python3 tests/r3lay/mock_supabase.py $PORT "$O/mock.log" &
    local mock=$!
    sleep 0.5
    printf 'url=http://127.0.0.1:%s\nkey=test-public-key\n' "${CONF_PORT:-$PORT}" > "$O/r3lay.conf"
    : > "$O/mock.log"
    R3LAY_CONF=${NO_CONF:-$PWD/$O/r3lay.conf} timeout 70 build-linux/lkx-shell --backend headless --disk "$O/disk.img" \
        --size 720x1280 --for 60 --keys "$P quit" --shot "$O/shot.png" > "$O/shell.log" 2>&1 < /dev/null
    kill $mock 2>/dev/null
    wait $mock 2>/dev/null
}

check() {
    local name=$1 ok=1 want file pattern
    shift
    for want in "$@"; do        # shell:REGEX, mock:REGEX, a leading ! on either: must not match
        pattern=${want#*:} file=$O/shell.log
        case $want in *mock:*) file=$O/mock.log ;; esac
        case $want in
        !*) grep -Eq "$pattern" "$file" && ok=0 ;;
        *)  grep -Eq "$pattern" "$file" || ok=0 ;;
        esac
    done
    if [ $ok = 1 ]; then
        echo "PASS  $name"
    else
        echo "FAIL  $name"
        failures=$((failures + 1))
        grep -E 'r3lay|crash|stopped' "$O/shell.log" | sed 's/^/      /'
        sed 's/^/      /' "$O/mock.log"
    fi
}

begin; login nope
run
check "a wrong password is refused" 'shell:r3lay: sign in failed: Wrong email' '!shell:r3lay: signed in'

begin; login good
run
check "signs in; the login is recorded" 'shell:r3lay: signed in' 'mock:"grant": "password"' 'mock:touch_login'
if strings "$O/disk.img" | grep -q 'refresh-1'; then
    echo "PASS  keeps the refresh token"
else
    echo "FAIL  keeps the refresh token"
    failures=$((failures + 1))
fi
if strings "$O/disk.img" | grep -qx 'good'; then
    echo "FAIL  the password isn't stored"
    failures=$((failures + 1))
else
    echo "PASS  the password isn't stored"
fi

begin; P+=" sleep:2"
run
check "stays signed in after closing" 'shell:r3lay: signed in' '!shell:r3lay: signing in' 'mock:"grant": "refresh_token"'

# Sending: bob's chat, a message, his reply through polling.
begin; P+=" sleep:2"; tap 360 344; P+=" sleep:2"; tap 300 1224; P+=" sleep:0.6"
typ hello; P+=" ret sleep:6"
run
check "sends with a client_ref; the reply arrives by polling" 'shell:r3lay: 7 messages' \
    'mock:"text": "hello".*"ref": "[0-9a-f-]{36}"' 'mock:marked_read'

# The send arrives but its answer is lost: no second copy.
begin; P+=" sleep:2"; tap 360 344; P+=" sleep:2"; tap 300 1224; P+=" sleep:0.6"
typ once; P+=" ret sleep:11"
MOCK_LOSE_REPLY=1 run
if [ "$(grep -c '"text": "once"' "$O/mock.log")" = 1 ]; then
    echo "PASS  a lost answer doesn't send it twice"
else
    echo "FAIL  a lost answer doesn't send it twice"
    failures=$((failures + 1))
    sed 's/^/      /' "$O/mock.log"
fi

# Signed in, no server: the app still opens, and says so.
begin; P+=" sleep:3"
CONF_PORT=1 run
check "no network: opens anyway" 'shell:r3lay: signed in' '!shell:crash|Segmentation'

# No r3lay.conf: it says so, and stays up.
begin; P+=" sleep:3 press:1 release sleep:2"
NO_CONF=/nonexistent run
check "not set up: says so and keeps running" "shell:r3lay: not set up" "!shell:crashed|Segmentation"

[ $failures = 0 ] && echo "all passed" || echo "$failures failed"
exit $failures
