#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_usb_hid"
mkdir -p "$out"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I "$root/tests/usb_host/stubs" -I "$root/components/aether_usb_host/include" \
 -I "$root/components/aether_usb_host" -I "$root/components/aether_usb_bus/include" \
 "$root/tests/usb_host/host_test.c" "$root/components/aether_usb_host/hid_report.c" \
 "$root/components/aether_usb_bus/aether_usb_bus.c" -o "$out/host_test"
"$out/host_test"
