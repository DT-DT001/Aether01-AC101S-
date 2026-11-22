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
constant_end = source.index("};", constants) + 2
start = source.index("static esp_err_t read_weather_body(")
end = source.index("void app_weather_get(", start)
(out / "weather_http.inc").write_text(
    source[constants:constant_end] + "\n" + source[start:end],
    encoding="utf-8",
)

def section(content, start, end):
    first = content.index(start)
    return content[first:content.index(end, first)]

header = (root / "main/app_utilities.h").read_text(encoding="utf-8")
tone = (root / "main/app_alarm_audio.h").read_text(encoding="utf-8")
rtc = (root / "components/pcf85063/include/pcf85063.h").read_text(encoding="utf-8")
(out / "alarm_types.inc").write_text("\n".join((
    section(tone, "enum { APP_ALARM_TONE_COUNT", "const char *"),
    section(header, "enum { APP_NOTE_COUNT", "esp_err_t app_utilities_init"),
    section(rtc, "typedef struct {", "esp_err_t pcf85063_new"),
)), encoding="utf-8")
(out / "alarm_service.inc").write_text("\n".join((
    source[constants:constant_end],
    section(source, "static const char s_namespace", "static app_power_t *s_power"),
    section(source, "static EXT_RAM_BSS_ATTR char s_notes", "static uint32_t s_note_revision"),
    section(source, "static app_alarm_snapshot_t s_alarm", "static bool s_city_pending"),
    section(source, "static esp_err_t load_value_error", "static esp_err_t save_note("),
    section(source, "static bool collect_due_alarms", "static void service("),
    section(source, "void app_alarm_get(", "void app_note_get_status("),
)), encoding="utf-8")
PY
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -I"$out" -I"$root/main" -I"$root/tests/player/stubs" \
    "$root/main/app_weather_data.c" "$root/main/app_tools_math.c" \
    "$root/tests/utilities/http_test.c" -lm -o "$out/http_test"
"$out/http_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -I"$out" -I"$root/main" -I"$root/tests/player/stubs" \
    "$root/main/app_tools_math.c" "$root/tests/utilities/alarm_test.c" -lm -o "$out/alarm_test"
"$out/alarm_test"
