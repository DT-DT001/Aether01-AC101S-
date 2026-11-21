"""Reuse existing native LVGL objects; never install tools or rebuild LVGL."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[2]
lvgl = root / "managed_components/lvgl__lvgl"
cached = root / "build/host_reader_ui"
objects = sorted(cached.glob("managed_components_lvgl__lvgl_src_*.c.o"))
if not objects or not any(path.name.endswith("_display_lv_display.c.o") for path in objects):
    raise SystemExit("Native LVGL objects are absent; run tests/reader_ui/build_run.py first.")

output = root / "build/host_power_tests"
output.mkdir(parents=True, exist_ok=True)
binary = output / "lvgl_semantics"
subprocess.run([
    os.environ.get("CC", "gcc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
    "-ffunction-sections", "-fdata-sections", "-DLV_CONF_INCLUDE_SIMPLE", "-D_DEFAULT_SOURCE",
    "-DLV_CONF_SUPPRESS_DEFINE_CHECK=1",
    "-I" + str(root / "tests/reader_ui"), "-I" + str(lvgl),
    str(root / "tests/power/lvgl_semantics_test.c"),
    *map(str, objects), "-Wl,--gc-sections", "-lm", "-o", str(binary),
], check=True)
subprocess.run([str(binary)], check=True)
