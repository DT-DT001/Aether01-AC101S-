#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_pinyin"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$root/main" "$root/main/app_pinyin.c" "$root/tests/pinyin/test.c" -o "$out/pinyin_test"
"$out/pinyin_test"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DAPP_PINYIN_EXTENDED_TEST -I"$root/main" "$root/main/app_pinyin.c" \
    "$root/main/app_pinyin_extended.c" "$root/tests/pinyin/extended_test.c" -o "$out/pinyin_extended_test"
"$out/pinyin_extended_test" "$@"
