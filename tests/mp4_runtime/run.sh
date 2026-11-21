#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="${AETHER_TEST_OUTPUT:-$project_root/build/host_mp4_runtime}"
player_source="${AETHER_MP4_PLAYER_SOURCE:-$project_root/components/aether_mp4_player/aether_mp4_player.c}"
mkdir -p "$output"
python3 "$test_root/generate.py" "$output"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -pthread -Wl,--wrap=free -Wl,--wrap=fread -Wl,--wrap=__fread_chk \
  -I "$test_root/stubs" -I "$project_root/tests/player/stubs" -I "$project_root/tests/aac_runtime/stubs" \
  -I "$project_root/tests/mjpeg_runtime/stubs" -I "$project_root/components/aether_mjpeg_player/include" \
  -I "$project_root/components/aether_mp4_player/include" -I "$project_root/components/aether_mp4_player" \
  "$project_root/components/aether_mp4_player/mp4_demux.c" "$project_root/components/aether_mp4_player/mp4_aac.c" \
  "$player_source" "$project_root/components/aether_mp4_player/mp4_audio.c" "$test_root/runtime_test.c" -o "$output/runtime_test"
"$output/runtime_test" "$output"
python3 "$test_root/run_chunk_cache.py"
python3 "$test_root/run_color.py"
