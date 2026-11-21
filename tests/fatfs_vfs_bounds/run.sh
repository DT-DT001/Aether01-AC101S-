#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_fatfs_vfs_bounds"
mkdir -p "$out"
python3 "$root/tests/fatfs_vfs_bounds/extract.py" "$root/components/fatfs/vfs/vfs_fat.c" "$out/production_vfs.c"
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror \
 -Wno-unused-parameter -Wno-sign-compare -fsanitize=address,undefined -fno-omit-frame-pointer \
 -I "$root/tests/fatfs_layout/stubs" -I "$root/components/fatfs/src" -I "$out" \
 "$root/tests/fatfs_vfs_bounds/bounds_test.c" -o "$out/test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$out/test"
