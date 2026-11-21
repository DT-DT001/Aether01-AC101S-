#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output_root="$project_root/build/host_power_tests"
mkdir -p "$output_root"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I "$test_root/stubs" \
    "$test_root/power_test.c" -o "$output_root/power_native"
"$output_root/power_native"
python3 "$test_root/extract_shutdown_cancel.py" "$output_root/shutdown_cancel.inc"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I "$test_root/stubs" -I "$output_root" "$test_root/shutdown_cancel_test.c" -o "$output_root/shutdown_cancel_native"
"$output_root/shutdown_cancel_native"
