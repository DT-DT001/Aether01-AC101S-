#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="${AETHER_TEST_OUTPUT:-$root/build/host_bluetooth_hid}"
mkdir -p "$output"
idf_api="${IDF_PATH:-/mnt/c/APP/ESP/.espressif/v6.1/esp-idf}/components/bt/host/bluedroid/api/include/api"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
  -DCONFIG_BT_HID_DEVICE_ENABLED=1 -I "$test_root/stubs" -I "$root/tests/bluetooth_runtime/stubs" \
  -I "$root/tests/player/stubs" -I "$idf_api" -I "$root/components/aether_bluetooth" \
  -I "$root/components/aether_bluetooth/include" "$test_root/hid_test.c" \
  "$root/components/aether_bluetooth/bt_hid.c" "$root/components/aether_bluetooth/bt_hid_reports.c" \
  -o "$output/hid_test"
UBSAN_OPTIONS=halt_on_error=1 "$output/hid_test"
