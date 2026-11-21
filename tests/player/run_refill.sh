#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="$project_root/build/host_music_refill"
mkdir -p "$output"
for bytes in 131072 32768; do
    "${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
      -DAETHER_MP3_IO_BYTES="$bytes" -Wl,--wrap=fread -Wl,--wrap=__fread_chk \
      -I "$test_root/stubs" -I "$project_root/components/aether_audio_player" \
      "$project_root/components/aether_audio_player/mp3_stream.c" "$test_root/refill_test.c" \
      -o "$output/refill_$bytes"
    "$output/refill_$bytes" "$bytes"
done
