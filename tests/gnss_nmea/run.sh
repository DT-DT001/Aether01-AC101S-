#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_gnss_nmea"
mkdir -p "$out"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -O1 -g \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    -I"$root/components/aether_gnss/include" \
    "$root/components/aether_gnss/gnss_nmea.c" "$root/tests/gnss_nmea/test.c" \
    -lm -o "$out/gnss_nmea_test"
"$out/gnss_nmea_test"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -O1 -g \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    -I"$root/components/aether_gnss/include" \
    "$root/components/aether_gnss/gnss_nmea.c" "$root/tests/gnss_nmea/casic_stream_test.c" \
    -lm -o "$out/casic_stream_test"
"$out/casic_stream_test"
