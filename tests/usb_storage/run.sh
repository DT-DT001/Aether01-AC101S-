#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_usb_storage"
mkdir -p "$out"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I "$root/tests/usb_storage/stubs" -I "$root/tests/usb_host/stubs" \
 -I "$root/components/aether_usb_storage/include" -I "$root/components/aether_usb_host/include" \
 "$root/tests/usb_storage/storage_test.c" -o "$out/storage_test"
"$out/storage_test"
