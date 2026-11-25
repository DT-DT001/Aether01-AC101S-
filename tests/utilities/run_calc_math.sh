#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_utilities"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$root/main/apps" "$root/main/apps/app_calc_modes.c" "$root/tests/utilities/calc_math_test.c" \
    -lm -o "$out/calc_math_test"
"$out/calc_math_test"
