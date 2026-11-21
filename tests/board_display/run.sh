#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output_root="$project_root/build/host_board_display_tests"
mkdir -p "$output_root"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$test_root/stubs" \
    "$project_root/components/aether_board/aether_board_display.c" \
    "$project_root/components/co5300_display/co5300_sleep.c" \
    "$test_root/display_test.c" -o "$output_root/display_test"
"$output_root/display_test"
