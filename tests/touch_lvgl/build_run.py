"""Compile actual app_touch.c against cached real LVGL pointer processing."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[2]
test = root / "tests/touch_lvgl"
lvgl = root / "managed_components/lvgl__lvgl"
objects = sorted((root / "build/host_reader_ui").glob("managed_components_lvgl__lvgl_src_*.c.o"))
if not objects:
    raise SystemExit("Run tests/reader_ui/build_run.py first to provide native LVGL objects.")
out = root / "build/host_touch_lvgl"
out.mkdir(parents=True, exist_ok=True)
binary = out / "touch_lvgl"
subprocess.run([
    os.environ.get("CC", "gcc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
    "-ffunction-sections", "-fdata-sections", "-DLV_CONF_INCLUDE_SIMPLE", "-D_DEFAULT_SOURCE",
    "-DLV_CONF_SUPPRESS_DEFINE_CHECK=1", "-I" + str(test / "stubs"),
    "-I" + str(root / "tests/reader_ui"), "-I" + str(lvgl), "-I" + str(root / "main"),
    str(root / "main/app_touch.c"), str(test / "touch_lvgl.c"),
    *map(str, objects), "-Wl,--gc-sections", "-lm", "-o", str(binary),
], check=True)
subprocess.run([str(binary)], check=True)
