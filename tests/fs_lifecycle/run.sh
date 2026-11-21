#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_fs_lifecycle"
mkdir -p "$out"
cc -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror -pthread \
 -fsanitize=address,undefined -fno-omit-frame-pointer \
 -I "$root/tests/fs_lifecycle/stubs" -I "$root/components/fatfs/vfs" \
 "$root/components/fatfs/vfs/vfs_fat_lifecycle.c" "$root/tests/fs_lifecycle/test.c" -o "$out/test"
"$out/test"
