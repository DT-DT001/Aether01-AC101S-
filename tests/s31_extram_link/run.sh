#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
python3 "$root/tests/s31_extram_link/test_link.py"
python3 "$root/tests/s31_extram_link/test_cmake.py"
