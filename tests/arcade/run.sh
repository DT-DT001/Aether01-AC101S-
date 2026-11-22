#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_arcade"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$root/main" "$root/main/app_arcade.c" "$root/tests/arcade/arcade_test.c" \
    -lm -o "$out/arcade_test"
"$out/arcade_test"
