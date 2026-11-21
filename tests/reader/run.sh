#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output_root="$project_root/build/host_reader_tests"
unicode_source=$(python3 "$test_root/fatfs_unicode.py")
mkdir -p "$output_root"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$project_root/main" "$project_root/main/app_reader_layout.c" \
    "$test_root/layout_test.c" -o "$output_root/reader_test"
"$output_root/reader_test"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$test_root/stubs" -I "$project_root/main" \
    -include "$test_root/stubs/ff.h" "$unicode_source" \
    "$project_root/main/app_reader_file.c" "$project_root/main/app_text.c" "$project_root/main/app_reader_layout.c" \
    "$test_root/file_test.c" -o "$output_root/reader_file_test"
"$output_root/reader_file_test"
python3 "$test_root/font_assets_test.py"
sh "$project_root/tests/reader_io/run.sh"
