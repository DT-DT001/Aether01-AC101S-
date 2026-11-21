#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_usb_storage_driver"
mkdir -p "$out"
python3 "$root/tests/usb_storage_driver/extract.py"
cc -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
 -fsanitize=address,undefined -fno-omit-frame-pointer -I "$out" \
 "$root/tests/usb_storage_driver/vfs_test.c" -o "$out/test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$out/test"

cc -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
 -fsanitize=address,undefined -fno-omit-frame-pointer -I "$out" \
 "$root/tests/usb_storage_driver/sd_test.c" -o "$out/sd_test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$out/sd_test"
