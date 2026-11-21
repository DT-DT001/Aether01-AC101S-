#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
vendor="$project_root/components/aether_nes/vendor/nofrendo"
output="$project_root/build/host_media_tests"
mkdir -p "$output"
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined \
  -I "$vendor" -I "$vendor/nes" -I "$vendor/sndhrdw" -I "$vendor/cpu" \
  "$test_root/save_test.c" -o "$output/nes_save"
"$output/nes_save" "$output"
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -Wl,--gc-sections \
  -I "$vendor" -I "$vendor/cpu" -I "$vendor/nes" -I "$vendor/sndhrdw" \
  "$test_root/core_loop_test.c" "$vendor/nes/nes.c" -o "$output/nes_core_loop"
"$output/nes_core_loop"
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -Wl,--gc-sections \
  -I "$test_root/stubs" -I "$project_root/components/aether_nes/include" \
  -I "$vendor" -I "$vendor/cpu" -I "$vendor/nes" -I "$vendor/sndhrdw" \
  "$test_root/port_test.c" "$vendor/bitmap.c" -o "$output/nes_port"
"$output/nes_port"
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -Wl,--gc-sections \
  -I "$test_root/stubs" -I "$project_root/components/aether_nes/include" \
  "$test_root/rom_header_test.c" -o "$output/nes_rom_header"
"$output/nes_rom_header" "$output"
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -Wl,--gc-sections \
  -I "$test_root/stubs" -I "$project_root/components/aether_nes/include" \
  "$test_root/worker_test.c" -o "$output/nes_worker"
"$output/nes_worker" "$output/worker-original.nes"
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -Wl,--gc-sections -Wl,--wrap=malloc \
  -I "$vendor" -I "$vendor/cpu" -I "$vendor/nes" -I "$vendor/sndhrdw" \
  "$test_root/config_test.c" "$project_root/components/aether_nes/aether_nes_save.c" -o "$output/nes_config"
"$output/nes_config" "$output"
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -Wl,--gc-sections \
  -I "$vendor" -I "$vendor/cpu" \
  "$test_root/cpu_word_test.c" -o "$output/nes_cpu_word"
UBSAN_OPTIONS=halt_on_error=1 "$output/nes_cpu_word"
objects="$output/nes_full_core_objects"
mkdir -p "$objects"
for source in "$test_root/full_core_test.c" "$project_root/components/aether_nes/aether_nes_save.c" "$vendor"/*.c "$vendor/cpu"/*.c \
              "$vendor/nes"/*.c "$vendor/sndhrdw"/*.c "$vendor/mappers"/*.c; do
  object="$objects/$(basename "$(dirname "$source")")_$(basename "$source").o"
  if [ ! -f "$object" ] || [ "$source" -nt "$object" ] || [ "$test_root/run.sh" -nt "$object" ] || \
     [ -n "$(find "$vendor" "$test_root/stubs" -name '*.h' -newer "$object" -print -quit)" ]; then
    "${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -ffunction-sections -fdata-sections \
      -fsanitize=address,undefined \
      -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-attributes -Wno-format-truncation \
      -I "$test_root/stubs" -I "$vendor" -I "$vendor/cpu" -I "$vendor/nes" -I "$vendor/sndhrdw" -I "$vendor/mappers" \
      -c "$source" -o "$object"
  fi
done
"${CC:-cc}" -fsanitize=address,undefined -Wl,--gc-sections "$objects"/*.o -lm -o "$output/nes_full_core"
UBSAN_OPTIONS=halt_on_error=1 "$output/nes_full_core" "$output"
