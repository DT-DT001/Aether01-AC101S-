#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_utilities"
mkdir -p "$out"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -I"$root/main" -I"$root/tests/player/stubs" \
    "$root/main/app_weather_data.c" "$root/main/app_tools_math.c" \
    "$root/tests/utilities/weather_test.c" -lm -o "$out/weather_test"
"$out/weather_test"
python3 - "$root" "$out" <<'PY'
from pathlib import Path
import sys

root, out = map(Path, sys.argv[1:])
source = (root / "main/app_utilities.c").read_text(encoding="utf-8")
constants = source.index("enum {\n    WEATHER_BODY")
start = source.index("static esp_err_t read_weather_body(")
end = source.index("void app_weather_get(", start)
(out / "weather_http.inc").write_text(
    source[constants:source.index("};", constants) + 2] + "\n" + source[start:end],
    encoding="utf-8",
)
PY
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -I"$out" -I"$root/main" -I"$root/tests/player/stubs" \
    "$root/main/app_weather_data.c" "$root/main/app_tools_math.c" \
    "$root/tests/utilities/http_test.c" -lm -o "$out/http_test"
"$out/http_test"
