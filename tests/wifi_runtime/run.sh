#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="${AETHER_TEST_OUTPUT:-$project_root/build/host_wifi_runtime}"
mkdir -p "$output"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -pthread -I "$test_root/stubs" -I "$project_root/tests/player/stubs" \
  -I "$project_root/components/aether_wifi/include" \
  "$project_root/components/aether_wifi/aether_wifi.c" "$test_root/test_wifi.c" -o "$output/test_wifi"
"$output/test_wifi"
