#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../../.." && pwd)
vendor="$test_root/../vendor/nofrendo"
output="$project_root/build/host_media_tests"
mkdir -p "$output/nes"
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -Wl,--gc-sections -Wl,--wrap=malloc -Wl,--wrap=free \
  -I "$vendor" -I "$vendor/cpu" -I "$vendor/nes" -I "$vendor/sndhrdw" \
  "$test_root/rom_bitmap_test.c" "$vendor/nes/nes_rom.c" "$vendor/bitmap.c" \
  "$project_root/components/aether_nes/aether_nes_save.c" \
  -o "$output/nes_rom_bitmap"
"$output/nes_rom_bitmap" "$output/nes"
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -Wl,--gc-sections \
  -I "$vendor" -I "$vendor/cpu" -I "$vendor/nes" -I "$vendor/sndhrdw" \
  "$test_root/gui_test.c" "$vendor/gui_elem.c" -o "$output/nes_gui"
"$output/nes_gui"
