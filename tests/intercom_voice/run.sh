#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="${AETHER_TEST_OUTPUT:-$root/build/host_intercom_voice}"
mkdir -p "$out"
python3 - "$root" "$out" <<'PY'
from pathlib import Path
import sys

root, out = map(Path, sys.argv[1:])
source = (root / 'main/app_intercom.c').read_text(encoding='utf-8')
types = source.index('#define FRAME_US')
state = source.index('static portMUX_TYPE s_lock', types)
voice = source.index('static void finish_voice(')
capture = source.index('static void begin_capture(', voice)
(out / 'voice-types.inc').write_text(source[types:state], encoding='utf-8')
(out / 'voice-production.inc').write_text(source[voice:capture], encoding='utf-8')
PY
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -I "$out" -I "$root/tests/player/stubs" -I "$root/tests/recorder/stubs" \
    -I "$root/components/aether_intercom_audio/include" -I "$root/components/aether_intercom_radio/include" \
    "$root/tests/intercom_voice/test.c" -o "$out/test"
"$out/test"
