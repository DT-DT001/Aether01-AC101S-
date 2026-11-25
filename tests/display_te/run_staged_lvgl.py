"""Real LVGL + production DMA staging; separate output from all other suites."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[2]
test = root / "tests/display_te"
lvgl = root / "managed_components/lvgl__lvgl"
objects = sorted((root / "build/host_reader_ui").glob("managed_components_lvgl__lvgl_src_*.c.o"))
if not objects:
    raise SystemExit("Run tests/reader_ui/build_run.py first for the existing real LVGL objects.")
out = root / "build/host_display_staged_lvgl_audit"
out.mkdir(exist_ok=True)
binary = out / "staged_lvgl_test"
subprocess.run([
    os.environ.get("CC", "gcc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
    "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=address,undefined",
    "-ffunction-sections", "-fdata-sections", "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_CONF_SUPPRESS_DEFINE_CHECK=1",
    "-I" + str(test / "staged_stubs"), "-I" + str(test / "stubs"),
    "-I" + str(root / "tests/reader_ui"), "-I" + str(lvgl), "-I" + str(root / "main"),
    "-I" + str(root / "components/co5300_display/include"),
    str(root / "main/app_display_metrics.c"), str(root / "main/app_display_sync.c"),
    str(root / "components/co5300_display/co5300_stream.c"),
    str(test / "staged_lvgl_test.c"), *map(str, objects), "-Wl,--gc-sections", "-lm", "-o", str(binary),
], check=True)
subprocess.run([str(binary)], check=True)
