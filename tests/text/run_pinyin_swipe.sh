#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_pinyin_swipe"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/main" "$root/main/app_pinyin_swipe.c" "$root/main/app_pinyin.c" \
    "$root/tests/text/pinyin_swipe_test.c" \
    -o "$out/test"
"$out/test"
