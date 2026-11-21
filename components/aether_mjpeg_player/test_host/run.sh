#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../../.." && pwd)
output="$project_root/build/host_media_tests"
mkdir -p "$output"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$test_root" -I "$test_root/.." "$test_root/stream_test.c" \
  "$test_root/../mjpeg_stream.c" -o "$output/mjpeg_stream"
"$output/mjpeg_stream"
