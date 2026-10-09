#!/usr/bin/env bash
# LiteKern X — users and their password hashes (kernel/users.c, kernel/sha256.c),
# tested on Linux with AddressSanitizer: SHA-256 against the standard
# vectors, adding, renaming, removing and checking users, users.txt written
# and read back, and its hashes worked out again by Python's hashlib (so the
# scheme in kernel/users.h is what's really stored).
#
# Usage: bash tests/users/test-users.sh   (or: make test-users)
set -uo pipefail
cd "$(dirname "$0")/../.."

mkdir -p build/test-users
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I. \
    -include stdio.h kernel/users.c kernel/sha256.c tests/users/users_host.c \
    -o build/test-users/users_host 2>&1 || exit 1
out=$(ASAN_OPTIONS=detect_leaks=0 build/test-users/users_host)
status=$?
grep '^users: ' <<<"$out"

python3 - "$out" <<'EOF' || status=1
import hashlib, sys
lines = sys.argv[1].split("file:\n", 1)[1].splitlines()
passwords = {"User": "hunter2", "Ben Jones": "pass word"}
ok = True
for l in lines:
    if not l.startswith("user "):
        continue
    _, role, rgb, salt, h, name = l.split(" ", 5)
    if name not in passwords:
        good = h == "-"
    else:
        s, pw = bytes.fromhex(salt), passwords[name].encode()
        d = hashlib.sha256(s + pw).digest()
        for _ in range(20000 - 1):
            d = hashlib.sha256(d + s + pw).digest()
        good = d.hex() == h
    print(("PASS  " if good else "FAIL  ") + f"hashlib agrees on {name}'s stored hash")
    ok &= good
sys.exit(0 if ok else 1)
EOF
echo
[ $status -eq 0 ] && echo "users: all tests passed" || { echo "users: FAILED"; exit 1; }
