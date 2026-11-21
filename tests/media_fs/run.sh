#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_media_fs"
mkdir -p "$out"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
 -I "$root/tests/media_fs/stubs" -I "$root/tests/usb_host/stubs" \
 -I "$root/components/aether_media_fs/include" -I "$root/components/aether_usb_storage/include" \
 -I "$root/components/aether_usb_host/include" \
 "$root/tests/media_fs/media_fs_test.c" "$root/components/aether_media_fs/aether_media_fs.c" -o "$out/test"
"$out/test"
