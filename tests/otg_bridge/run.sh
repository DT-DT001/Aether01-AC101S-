#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
out="$project_root/build/host_otg_bridge"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -pthread -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$test_root/stubs" -I "$project_root/tests/tg29/stubs" \
    -I "$project_root/components/tg29/include" \
    -I "$project_root/components/aether_usb_host/include" -I "$project_root/main" \
    "$test_root/bridge_test.c" -o "$out/bridge_test"
"$out/bridge_test"
