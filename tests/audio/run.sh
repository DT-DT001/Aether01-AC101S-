#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output_root="${AETHER_TEST_OUTPUT:-$project_root/build/host_audio_tests}"
mkdir -p "$output_root"
"${CC:-cc}" -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
    -I "$test_root/stubs" "$test_root/ac101s_register_test.c" -o "$output_root/ac101s_register_native"
"$output_root/ac101s_register_native"
"${CC:-cc}" -std=c11 -O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections -Wall -Wextra -Werror \
    -I "$test_root/stubs" -I "$project_root/components/ac101s_audio/include" \
    "$test_root/audio_test.c" -Wl,--gc-sections -lm -o "$output_root/audio_native"
"$output_root/audio_native"
"${CC:-cc}" -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
    -I "$test_root/stubs" \
    -I "$project_root/components/ac101s_audio/include" -I "$project_root/main" \
    -I "$project_root/tests/player/stubs" \
    "$project_root/main/app_alarm_audio.c" "$test_root/alarm_audio_test.c" -o "$output_root/alarm_audio_native"
"$output_root/alarm_audio_native"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -pthread -Wall -Wextra -Werror \
    -I "$test_root/stubs" -I "$project_root/components/ac101s_audio/include" \
    "$test_root/route_test.c" -Wl,--gc-sections -lm -o "$output_root/route_native"
"$output_root/route_native"
"${CC:-cc}" -std=c11 -O1 -g -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -pthread -Wall -Wextra -Werror \
    -I "$test_root/stubs" -I "$project_root/components/ac101s_audio/include" \
    "$test_root/capture_test.c" -Wl,--gc-sections -lm -o "$output_root/capture_native"
"$output_root/capture_native"
"${CC:-cc}" -std=c11 -O1 -g -fsanitize=address,undefined -pthread -Wall -Wextra -Werror \
    -I "$test_root/stubs" -I "$project_root/components/ac101s_audio/include" \
    -I "$project_root/components/aether_bluetooth/include" -I "$project_root/main" \
    -I "$project_root/components/aether_usb_audio/include" \
    "$test_root/system_test.c" -o "$output_root/system_native"
"$output_root/system_native"
"${CC:-cc}" -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
    -I "$test_root/diagnostic_stubs" -I "$test_root/stubs" \
    -I "$project_root/components/ac101s_audio/include" -I "$project_root/main" \
    "$test_root/diagnostic_test.c" -lm -o "$output_root/diagnostic_native"
"$output_root/diagnostic_native"
