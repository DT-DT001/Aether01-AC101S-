#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="$project_root/build/host_media_control_tests"
mkdir -p "$output"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined \
    -I "$test_root/stubs" -I "$project_root/main" \
    -I "$project_root/components/aether_audio_player/include" \
    -I "$project_root/components/aether_video_player/include" \
    -I "$project_root/components/aether_mp4_player/include" \
    -I "$project_root/components/aether_mjpeg_player/include" \
    -I "$project_root/components/aether_nes/include" \
    -I "$project_root/components/aether_bluetooth/include" \
    "$test_root/media_control_test.c" "$project_root/main/app_media_control.c" \
    -o "$output/media_control_test"
UBSAN_OPTIONS=halt_on_error=1 "$output/media_control_test"
