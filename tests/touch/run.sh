#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output_root="$project_root/build/host_touch_tests"
mkdir -p "$output_root"
"${CC:-cc}" -std=c11 -O1 -Wall -Wextra -Werror -I "$test_root/stubs" -I "$project_root/main" \
    "$test_root/touch_test.c" -o "$output_root/touch_native"
"$output_root/touch_native"
