#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output_root="$project_root/build/host_player_tests"
mkdir -p "$output_root"
python3 "$project_root/tests/mp4_runtime/generate.py" "$project_root/build/host_mp4_runtime"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -pthread \
    -I "$test_root/stubs" -I "$project_root/components/aether_audio_player/include" \
    -I "$project_root/components/aether_audio_player" \
    -I "$project_root/components/aether_mp4_player/include" -I "$project_root/components/aether_mp4_player" \
    -I "$project_root/tests/aac_runtime/stubs" \
    "$test_root/player_test.c" "$project_root/components/aether_audio_player/wav_stream.c" \
    "$project_root/components/aether_audio_player/mp3_stream.c" "$project_root/components/aether_audio_player/music_eq.c" \
    "$project_root/components/aether_mp4_player/mp4_demux.c" \
    "$project_root/components/aether_mp4_player/mp4_aac.c" \
    "$project_root/components/aether_mp4_player/mp4_audio.c" "$test_root/aac_sdk_double.c" \
    -lm -o "$output_root/player_test"
"$output_root/player_test"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$test_root/stubs" -I "$project_root/components/aether_audio_player/include" \
    -I "$project_root/components/aether_audio_player" \
    "$test_root/eq_test.c" "$project_root/components/aether_audio_player/music_eq.c" \
    -lm -o "$output_root/eq_test"
"$output_root/eq_test"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined \
    -I "$test_root/stubs" -I "$project_root/components/aether_audio_player" \
    "$test_root/mp3_format_test.c" "$project_root/components/aether_audio_player/mp3_stream.c" \
    -o "$output_root/mp3_format_test"
"$output_root/mp3_format_test"
