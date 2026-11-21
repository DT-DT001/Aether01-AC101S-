#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_nes_controls"
mkdir -p "$out"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
 -I "$root/tests/nes_controls/stubs" -I "$root/tests/audio/stubs" -I "$root/main" \
 "$root/tests/nes_controls/controls_test.c" -o "$out/controls_test"
"$out/controls_test"
