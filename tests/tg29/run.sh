#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output_root="$project_root/build/host_tg29_tests"
mkdir -p "$output_root"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$test_root/stubs" -I "$project_root/components/tg29/include" \
    "$test_root/tg29_test.c" -o "$output_root/tg29_native"
"$output_root/tg29_native"
