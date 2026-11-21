#!/bin/sh
set -eu
frames_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$frames_root/../.." && pwd)
output_root="$project_root/build/host_frames_tests"
mkdir -p "$output_root"
"${CC:-cc}" -std=c11 -O1 -ffreestanding -fno-builtin -Wall -Wextra -Werror \
    -I "$frames_root/stubs" -I "$project_root/main" \
    "$project_root/main/app_frames.c" "$frames_root/frames_test.c" \
    "$frames_root/native_runner.c" -o "$output_root/frames_native"
"$output_root/frames_native"
