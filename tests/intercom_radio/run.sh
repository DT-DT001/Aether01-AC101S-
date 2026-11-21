#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="${AETHER_TEST_OUTPUT:-$root/build/host_intercom_radio}"
mkdir -p "$out"
python3 - "$root" "$out" <<'PY'
from pathlib import Path
import sys

root, out = map(Path, sys.argv[1:])
source = (root / 'components/aether_intercom_radio/aether_intercom_radio.c').read_text(encoding='utf-8')
lines = source.splitlines()
definitions = (
    'TX_AUDIO_CAPACITY', 'TX_CAPACITY', 'TX_AUDIO_MAX_AGE_US',
    'TX_AUDIO_RETRY_AGE_US', 'TX_RETRY_PAUSE_US', 'FAST_RATE_RETRY_US',
)
chunks = [line for line in lines if line.startswith(tuple(f'#define {name} ' for name in definitions))]
chunks.append(source[source.index('typedef struct {\n    aether_intercom_packet_t packet;'):
                     source.index('typedef struct {\n    uint8_t version;')])
(out / 'radio-timing-types.inc').write_text('\n'.join(chunks), encoding='utf-8')
chunks = (
    ('static bool voice_expired(', 'static void reset_metrics('),
    ('static void remove_expired_queued_audio(', 'static void wipe('),
    ('static bool service_voice_tx(', 'static void service_tx('),
)
(out / 'radio-timing-production.inc').write_text('\n'.join(
    source[source.index(start):source.index(end)] for start, end in chunks), encoding='utf-8')
PY
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
    -I "$out" -I "$root/tests/player/stubs" -I "$root/components/aether_intercom_radio/include" \
    "$root/tests/intercom_radio/test.c" -o "$out/test"
"$out/test"
