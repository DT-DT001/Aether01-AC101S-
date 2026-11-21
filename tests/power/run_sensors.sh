#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="$project_root/build/host_power_sensors"
mkdir -p "$output"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$test_root/sensors_stubs" -I "$project_root/tests/tg29/stubs" \
  -I "$project_root/components/qmi8658a/include" -I "$project_root/components/dps368/include" \
  "$project_root/components/qmi8658a/qmi8658a.c" "$project_root/components/dps368/dps368.c" \
  "$test_root/sensors_test.c" -lm -o "$output/sensors_native"
"$output/sensors_native"
