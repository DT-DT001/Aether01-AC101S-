#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$repo/build/host_usb_bus"
mkdir -p "$out"
cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
  -I"$repo/tests/usb_msc/stubs" -I"$repo/components/aether_usb_bus/include" \
  "$repo/tests/usb_bus/bus_test.c" "$repo/components/aether_usb_bus/aether_usb_bus.c" -o "$out/bus_test"
"$out/bus_test"
