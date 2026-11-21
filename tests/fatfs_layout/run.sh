#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_fatfs_layout"
mkdir -p "$out"
cc -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror \
 -fsanitize=address,undefined -fno-omit-frame-pointer \
 -I "$root/tests/fatfs_layout/stubs" -I "$root/components/fatfs/src" -I "$root/components/fatfs/diskio" \
 "$root/tests/fatfs_layout/layout_test.c" "$root/components/fatfs/src/ff.c" \
 "$root/components/fatfs/src/ffunicode.c" "$root/components/fatfs/diskio/diskio.c" -o "$out/test"
"$out/test"
