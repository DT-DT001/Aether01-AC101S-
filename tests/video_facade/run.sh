#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="$project_root/build/host_video_facade"
mkdir -p "$output"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -pthread \
  -I "$project_root/tests/player/stubs" -I "$project_root/tests/mjpeg_runtime/stubs" \
  -I "$project_root/components/aether_mjpeg_player/include" \
  -I "$project_root/components/aether_mp4_player/include" \
  -I "$project_root/components/aether_video_player/include" \
  "$test_root/facade_test.c" -o "$output/facade_test"
"$output/facade_test"
