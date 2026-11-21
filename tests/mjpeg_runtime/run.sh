#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="$project_root/build/host_mjpeg_runtime"
mkdir -p "$output"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -pthread -Wl,--wrap=free \
  -I "$test_root/stubs" -I "$project_root/tests/player/stubs" \
  -I "$project_root/components/aether_mjpeg_player/include" \
  -I "$project_root/components/aether_mjpeg_player" \
  "$test_root/runtime_test.c" "$project_root/components/aether_mjpeg_player/mjpeg_stream.c" \
  -o "$output/runtime_test"
"$output/runtime_test" "$test_root/frame.jpg"
