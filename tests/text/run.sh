#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output_root="$project_root/build/host_text_tests"
mkdir -p "$output_root"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$project_root/main" "$project_root/main/app_text.c" \
    "$test_root/text_test.c" -o "$output_root/text_test"
"$output_root/text_test"
