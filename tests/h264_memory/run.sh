#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_h264_memory"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -Wmissing-prototypes \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$root/tests/h264_memory/stubs" \
  "$root/components/aether_mp4_player/h264_hp_alloc.c" \
  "$root/tests/h264_memory/alloc_test.c" -o "$out/alloc_test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$out/alloc_test"
python3 "$root/tests/h264_memory/test_prepare.py"
python3 "$root/tests/h264_memory/test_link_order.py"
