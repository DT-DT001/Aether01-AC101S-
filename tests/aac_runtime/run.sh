#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="$root/build/host_aac_runtime"
mkdir -p "$output"
python3 "$test_root/generate.py" --check
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$test_root/stubs" -I "$root/components/aether_mp4_player" \
    "$root/components/aether_mp4_player/mp4_aac.c" "$test_root/aac_test.c" -o "$output/aac_adapter"
UBSAN_OPTIONS=halt_on_error=1 "$output/aac_adapter"
if [ -n "${MAAC_HEADER_DIR:-}" ]; then
    "${CC:-cc}" -std=c11 -O1 -g -fsanitize=address,undefined \
        -I "$MAAC_HEADER_DIR" "$test_root/oracle.c" -lm -o "$output/real_fixture_oracle"
    UBSAN_OPTIONS=halt_on_error=1 "$output/real_fixture_oracle"
fi
