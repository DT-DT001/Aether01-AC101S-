#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="$project_root/build/host_power_telemetry"
mkdir -p "$output"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$test_root/telemetry_stubs" -I "$project_root/tests/tg29/stubs" \
  "$test_root/telemetry_test.c" -lm -o "$output/telemetry_native"
"$output/telemetry_native"
