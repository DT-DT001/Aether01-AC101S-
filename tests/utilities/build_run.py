"""Exercise utility screens, touch-to-talk, and adaptive keyboard using real LVGL."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
parser.add_argument("--lvgl-cache", type=Path)
parser.add_argument("--layout-only", action="store_true", help="Run calendar and touch-to-talk checks")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
test = root / "tests/utilities"
out = root / ("build/host_utilities_layout" if args.layout_only else "build/host_utilities_ui")
out.mkdir(parents=True, exist_ok=True)
lvgl = root / "managed_components/lvgl__lvgl"

if os.name == "nt" and Path(args.cc).is_absolute():
    os.environ["PATH"] = str(Path(args.cc).parent) + os.pathsep + os.environ["PATH"]
flags = [
    args.cc, "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
    "-ffunction-sections", "-fdata-sections", "-DLV_CONF_INCLUDE_SIMPLE",
    "-DLV_LVGL_H_INCLUDE_SIMPLE", "-D_DEFAULT_SOURCE", "-DLV_USE_CALENDAR_CHINESE=1",
    "-DLV_FONT_MONTSERRAT_20=1", "-DLV_FONT_MONTSERRAT_28=1", "-DLV_FONT_MONTSERRAT_48=1",
    *["-I" + str(path) for path in [
        test / "ui_stubs", out, root / "tests/player/stubs", root / "tests/reader_ui",
        root / "main/ui", root / "main", root / "main/apps", root / "components/aether_wifi/include", lvgl,
        root / "components/aether_intercom_radio/include",
    ]],
]
work = []
objects = []

def schedule(source, name, force=False):
    cached = args.lvgl_cache / name if args.lvgl_cache else None
    if not force and cached and cached.exists():
        objects.append(cached)
    else:
        obj = out / name
        objects.append(obj)
        work.append((source, obj))

for source in [*sorted((lvgl / "src").rglob("*.c")),
               root / "main/assets/font_16.c", root / "main/assets/font_24.c"]:
    name = source.relative_to(root).as_posix().replace("/", "_") + ".o"
    schedule(source, name, source.name in {
        "lv_calendar.c", "lv_calendar_chinese.c", "lv_font_montserrat_20.c",
        "lv_font_montserrat_28.c", "lv_font_montserrat_48.c",
    })
shutil.copyfile(root / "main/app_utilities.h", out / "app_utilities.h")
shutil.copyfile(root / "main/app_intercom.h", out / "app_intercom.h")
for source in [root / "main/apps/app_utilities.c", root / "main/apps/app_wifi.c", root / "main/apps/app_intercom.c",
               root / "main/ui/app_text_input.c"]:
    copy = out / source.name
    shutil.copyfile(source, copy)
    schedule(copy, copy.stem + ".o", True)
for source in [root / "main/app_weather_data.c", root / "main/app_tools_math.c",
               root / "main/app_text.c", root / "main/app_pinyin.c", root / "main/app_pinyin_swipe.c",
               test / "ui_smoke.c"]:
    schedule(source, source.stem + ".o", True)

def compile_one(job):
    source, obj = job
    subprocess.run([*flags, "-c", str(source), "-o", str(obj)], check=True)

with ThreadPoolExecutor(max_workers=8) as pool:
    list(pool.map(compile_one, work))
binary = out / ("utilities_ui.exe" if os.name == "nt" else "utilities_ui")
response = out / "link.rsp"
response.write_text("".join(f'"{obj.resolve().as_posix()}"\n' for obj in objects), encoding="utf-8")
subprocess.run([args.cc, "-Wl,--gc-sections", "@" + str(response), "-lm", "-o", str(binary)], check=True)
subprocess.run([str(binary), *(["--layout-only"] if args.layout_only else [])], check=True)
