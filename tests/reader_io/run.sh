#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output_root="$project_root/build/host_reader_io_tests"
mkdir -p "$output_root"
unicode_source=$(python3 "$project_root/tests/reader/fatfs_unicode.py")
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -include "$test_root/stubs/compat.h" \
    -include "$project_root/tests/reader/stubs/ff.h" "$unicode_source" \
    -I "$test_root/stubs" -I "$project_root/tests/reader_ui/stubs" -I "$project_root/tests/reader/stubs" -I "$project_root/main" \
    -I "$project_root/components/aether_recorder/include" -I "$project_root/components/aether_intercom_radio/include" \
    -I "$project_root/components/aether_usb_msc/include" -I "$project_root/components/aether_usb_bus/include" \
    -I "$project_root/components/aether_image/include" \
    "$test_root/io_test.c" "$project_root/components/aether_usb_bus/aether_usb_bus.c" "$project_root/main/app_reader_file.c" "$project_root/main/app_text.c" \
    -o "$output_root/io_test"
"$output_root/io_test"
