#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test="$root/tests/gps_service"
out="$root/build/host_gps_service"
mkdir -p "$out"
${CC:-cc} -std=c11 -pthread -Wall -Wextra -Werror -O1 -g -fno-omit-frame-pointer \
    -fsanitize=address,undefined \
    -I"$test/stubs" -I"$root/tests/otg_bridge/stubs" -I"$root/tests/tg29/stubs" \
    -I"$root/components/tg29/include" -I"$root/components/aether_usb_host/include" \
    -I"$root/components/aether_gnss/include" -I"$root/components/board_config/include" \
    -I"$root/main" "$test/service_test.c" "$root/components/aether_gnss/gnss_nmea.c" \
    -lm -o "$out/service_test"
"$out/service_test"
