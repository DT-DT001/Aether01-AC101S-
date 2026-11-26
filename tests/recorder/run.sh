#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="$project_root/build/host_recorder_tests"
mkdir -p "$output"
mount=$(mktemp -d /tmp/aether-recorder-XXXXXX)
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -pthread \
    -DAETHER_STORAGE_MOUNT_POINT=\""$mount"\" -DAETHER_RECORDER_HAS_NSNET3 \
    -I "$test_root/stubs" -I "$project_root/components/aether_recorder/include" \
    "$test_root/recorder_test.c" "$project_root/components/aether_recorder/aether_recorder_denoise.c" \
    -o "$output/recorder_test"
"$output/recorder_test"
printf 'Original generated WAV fixtures retained at %s\n' "$mount"
