#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"${PYTHON:-python3}" "$test_root/build_run.py"
