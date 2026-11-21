#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$repo/build/host_usb_msc"
mkdir -p "$out"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
  -I"$repo/tests/usb_msc/stubs" -I"$repo/tests/audio/stubs" -I"$repo/components/aether_usb_msc/include" -I"$repo/components/aether_usb_bus/include" \
  "$repo/tests/usb_msc/usb_test.c" "$repo/components/aether_usb_bus/aether_usb_bus.c" -o "$out/usb_test"
"$out/usb_test"
"$out/usb_test" --mac-read-error
