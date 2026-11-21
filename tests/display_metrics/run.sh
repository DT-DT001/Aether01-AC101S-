#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$test_root/../.." && pwd)
output="${AETHER_TEST_OUTPUT:-$project_root/build/host_display_metrics}"
mkdir -p "$output"
"${CC:-cc}" -std=c11 -O1 -g -pthread -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$test_root/stubs" -I "$project_root/main" \
    "$project_root/main/app_display_metrics.c" "$test_root/metrics_test.c" "$test_root/heap_stub.c" \
    -Wl,--wrap=free -o "$output/metrics_test"
"$output/metrics_test"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -pthread -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$test_root/stubs" -I "$project_root/main" \
    "$project_root/main/app_display_metrics.c" "$test_root/wait_test.c" "$test_root/heap_stub.c" \
    -Wl,--wrap=free -o "$output/wait_test"
"$output/wait_test"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -pthread -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$test_root/stubs" -I "$project_root/main" \
    "$project_root/main/app_display_metrics.c" "$test_root/staged_test.c" "$test_root/heap_stub.c" \
    -Wl,--wrap=free -o "$output/staged_test"
"$output/staged_test"
