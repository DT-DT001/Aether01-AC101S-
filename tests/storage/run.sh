#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output_root="$project_root/build/host_storage_tests"
mkdir -p "$output_root"
"${CC:-cc}" -std=c11 -O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections -Wall -Wextra -Werror \
    -I "$test_root/stubs" -I "$project_root/components/aether_storage/include" -I "$test_root/../audio/stubs" \
    "$test_root/storage_test.c" "$project_root/components/aether_storage/storage_probe.c" -Wl,--gc-sections -o "$output_root/storage_native"
"$output_root/storage_native"
python3 "$test_root/check_sdk_dma.py" "$output_root"

"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$project_root/components/aether_storage/include" \
    "$test_root/probe_test.c" "$project_root/components/aether_storage/storage_probe.c" \
    -o "$output_root/probe_native"
"$output_root/probe_native"
