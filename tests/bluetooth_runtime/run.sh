#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="${AETHER_TEST_OUTPUT:-$root/build/host_bluetooth_runtime}"
mkdir -p "$output"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$root/components/aether_bluetooth" "$test_root/pcm_test.c" \
    "$root/components/aether_bluetooth/bt_pcm.c" -o "$output/pcm_test"
UBSAN_OPTIONS=halt_on_error=1 "$output/pcm_test"
idf_api="${IDF_PATH:-/mnt/c/APP/ESP/.espressif/v6.1/esp-idf}/components/bt/host/bluedroid/api/include/api"
if [ ! -f "$idf_api/esp_a2dp_api.h" ]; then
    echo "Set IDF_PATH to the existing ESP-IDF 6.1 tree for production manager API tests" >&2
    exit 1
fi
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -pthread -DCONFIG_BT_HID_DEVICE_ENABLED=1 \
    -I "$test_root/stubs" -I "$root/tests/bluetooth_hid/stubs" -I "$root/tests/player/stubs" -I "$idf_api" \
    -I "$root/components/aether_bluetooth/include" -I "$root/components/aether_bluetooth" \
    "$test_root/bluetooth_test.c" "$root/components/aether_bluetooth/bt_pcm.c" \
    "$root/components/aether_bluetooth/bt_hid.c" "$root/components/aether_bluetooth/bt_hid_reports.c" \
    -o "$output/bluetooth_test"
UBSAN_OPTIONS=halt_on_error=1 "$output/bluetooth_test"
