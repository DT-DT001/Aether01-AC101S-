#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out="$root/build/host_user_key_ui"
mkdir -p "$out"
python3 - "$root" "$out" <<'PY'
from pathlib import Path
import sys
r=Path(sys.argv[1]);o=Path(sys.argv[2])
s=(r/'main/ui/app_ui.c').read_text()
a=s.index('void app_ui_set_user_key_exit(');b=s.index('lv_obj_t *app_ui_new_screen(',a)
(o/'user_key_selected.inc').write_text(s[a:b])
PY
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I "$out" "$root/tests/user_key_ui/test.c" -o "$out/test"
"$out/test"
