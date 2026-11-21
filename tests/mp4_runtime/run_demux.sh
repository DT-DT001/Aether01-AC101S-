#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="${AETHER_TEST_OUTPUT:-$project_root/build/host_mp4_runtime}"
mkdir -p "$output"
python3 "$test_root/generate.py" "$output"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
  -I "$test_root/stubs" -I "$project_root/tests/player/stubs" \
  -I "$project_root/components/aether_mp4_player" \
  "$project_root/components/aether_mp4_player/mp4_demux.c" "$test_root/demux_test.c" -o "$output/demux_test"
"$output/demux_test" "$output"
