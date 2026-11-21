#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="$project_root/build/host_display_te"
mkdir -p "$output"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -pthread -I "$test_root/stubs" \
  -I "$project_root/components/co5300_display/include" -I "$project_root/components/co5300_display" \
  "$project_root/components/co5300_display/co5300_te.c" "$test_root/te_test.c" -o "$output/te_test"
"$output/te_test"
python3 "$test_root/run_lvgl.py"
