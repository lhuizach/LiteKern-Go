#!/usr/bin/env bash
# platform/touch.c by itself. Usage (in WSL): bash tests/linux/test-touch.sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build-linux
gcc -std=gnu11 -Wall -Wextra -Werror -I. -o build-linux/test-touch tests/linux/test-touch.c platform/touch.c
build-linux/test-touch
