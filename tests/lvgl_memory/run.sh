#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="${AETHER_TEST_OUTPUT:-$root/build/host_lvgl_memory}"
lvgl="$root/managed_components/lvgl__lvgl"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
  -Wall -Wextra -Werror -Wmissing-prototypes -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DLV_CONF_SKIP=1 -DLV_USE_STDLIB_MALLOC=LV_STDLIB_CUSTOM \
  -DLV_USE_STDLIB_STRING=LV_STDLIB_CLIB -DLV_USE_LOG=0 -DLV_USE_OS=LV_OS_NONE \
  -I "$root/tests/lvgl_memory/stubs" -I "$lvgl" \
  "$root/main/app_lvgl_memory.c" "$lvgl/src/stdlib/lv_mem.c" \
  "$lvgl/src/stdlib/clib/lv_string_clib.c" "$root/tests/lvgl_memory/memory_test.c" \
  -Wl,--gc-sections -o "$out/memory_test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$out/memory_test"
