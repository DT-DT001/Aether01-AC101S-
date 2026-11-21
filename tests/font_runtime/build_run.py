"""Reuse native reader-UI LVGL objects; compile missing objects in a private cache."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import shlex
import subprocess

root = Path(__file__).resolve().parents[2]
test = root / "tests/font_runtime"
configuration = root / "tests/reader_ui"
lvgl = root / "managed_components/lvgl__lvgl"
shared = root / "build/host_reader_ui"
output = root / "build/host_font_runtime"
objects = output / "objects"
objects.mkdir(parents=True, exist_ok=True)
compiler = shlex.split(os.environ.get("CC", "gcc"))
flags = ["-std=c11", "-O0", "-g", "-UNDEBUG", "-ffunction-sections", "-fdata-sections",
         "-DLV_CONF_INCLUDE_SIMPLE", "-D_DEFAULT_SOURCE", "-I" + str(configuration / "stubs"),
         "-I" + str(configuration), "-I" + str(root / "main"), "-I" + str(lvgl)]
sources = sorted((lvgl / "src").rglob("*.c")) + [
    root / "main/assets/font_16.c", root / "main/assets/font_24.c", test / "font_test.c"]
headers = list(lvgl.rglob("*.h")) + [configuration / "lv_conf.h"]
dependency_time = max(path.stat().st_mtime_ns for path in headers)


def build_source(source: Path) -> tuple[Path, bool]:
    name = str(source.relative_to(root)).replace("/", "_") + ".o"
    newest = max(source.stat().st_mtime_ns, dependency_time)
    # Never alter the reader UI's cache, which another suite may be using.
    cached = shared / name
    if source != test / "font_test.c" and cached.exists() and cached.stat().st_mtime_ns >= newest:
        return cached, True
    private = objects / name
    if not private.exists() or private.stat().st_mtime_ns < max(newest, Path(__file__).stat().st_mtime_ns):
        subprocess.run(compiler + flags + ["-c", str(source), "-o", str(private)],
                       check=True, capture_output=True)
    return private, False


try:
    with ThreadPoolExecutor(max_workers=8) as pool:
        built = list(pool.map(build_source, sources))
except subprocess.CalledProcessError as error:
    print(error.stderr.decode(errors="replace"))
    raise

print(f"Font runtime: reused {sum(reused for _, reused in built)} native reader-UI objects; "
      "missing/stale objects use a separate font-runtime cache", flush=True)
binary = output / "font_sweep"
subprocess.run(compiler + ["-Wl,--gc-sections", *[str(path) for path, _ in built], "-lm", "-o", str(binary)],
               check=True)
subprocess.run([str(binary)], check=True)
