#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_motion"
mkdir -p "$out"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I"$root/main" "$root/main/app_motion_filter.c" "$root/tests/motion/filter_test.c" -lm -o "$out/filter_test"
"$out/filter_test"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I"$root/main" \
    "$root/main/app_displacement_estimator.c" "$root/tests/motion/displacement_test.c" -lm -o "$out/displacement_test"
"$out/displacement_test"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -pthread -fsanitize=address,undefined \
    -I"$root/tests/power/telemetry_stubs" -I"$root/tests/tg29/stubs" -I"$root/main" \
    "$root/main/app_displacement.c" "$root/main/app_displacement_estimator.c" \
    "$root/tests/motion/displacement_session_test.c" -lm -o "$out/displacement_session_test"
"$out/displacement_session_test"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I"$root/main" \
    "$root/main/app_air_mouse.c" "$root/tests/motion/air_mouse_test.c" -lm -o "$out/air_mouse_test"
"$out/air_mouse_test"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -pthread -fsanitize=address,undefined \
    -I"$root/tests/power/telemetry_stubs" -I"$root/tests/tg29/stubs" \
    -I"$root/tests/bluetooth_runtime/stubs" -I"$root/tests/nes_controls/stubs" \
    -I"$root/components/aether_bluetooth/include" -I"$root/main" \
    "$root/main/app_air_mouse.c" "$root/main/app_pc_remote.c" \
    "$root/tests/motion/pc_remote_test.c" -lm -o "$out/pc_remote_test"
"$out/pc_remote_test"
