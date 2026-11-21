#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output_root="${AETHER_TEST_OUTPUT:-$project_root/build/host_intercom_audio_tests}"
voice_root="$project_root/components/aether_intercom_audio"
mkdir -p "$output_root"
"${CC:-cc}" -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
    -I "$project_root/tests/audio/stubs" -I "$project_root/components/ac101s_audio/include" \
    -I "$voice_root/include" "$test_root/codec_test.c" "$voice_root/aether_intercom_audio.c" \
    -o "$output_root/codec_test"
"$output_root/codec_test"
