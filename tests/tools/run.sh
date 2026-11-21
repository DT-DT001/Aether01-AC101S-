#!/bin/sh
set -eu
repo="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$repo/main" "$repo/main/app_tools_math.c" \
    "$repo/tests/tools/tools_math_test.c" -lm -o "$out/tools_test"
"$out/tools_test"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -pthread \
    -I"$repo/tests/tools/stubs" -I"$repo/tests/player/stubs" \
    -I"$repo/components/aether_audio_player/include" -I"$repo/main" \
    "$repo/tests/tools/tools_worker_test.c" "$repo/main/app_tools_math.c" -lm -o "$out/tools_worker_test"
"$out/tools_worker_test"
