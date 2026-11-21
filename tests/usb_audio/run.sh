#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$root/build/host_usb_audio"
${CC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/tests/usb_audio/stubs" -I "$root/components/aether_usb_audio/include" -I "$root/components/aether_usb_audio" "$root/tests/usb_audio/audio_test.c" "$root/components/aether_usb_audio/pcm_convert.c" -o "$root/build/host_usb_audio/test"
"$root/build/host_usb_audio/test"
python3 "$root/tests/usb_audio/extract_vendor_cleanup.py" "$root/build/host_usb_audio/vendor_cleanup.inc"
${CC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/tests/usb_audio/stubs" -I "$root/build/host_usb_audio" "$root/tests/usb_audio/vendor_cleanup_test.c" -o "$root/build/host_usb_audio/vendor_cleanup_test"
"$root/build/host_usb_audio/vendor_cleanup_test"
