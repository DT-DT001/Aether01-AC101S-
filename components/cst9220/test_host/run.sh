#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../../.." && pwd)
output_root="$project_root/build/host_cst9220_tests"
mkdir -p "$output_root"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$test_root/stubs" -I "$test_root/../include" "$test_root/cst9220_test.c" \
    -o "$output_root/cst9220_test"
"$output_root/cst9220_test"
