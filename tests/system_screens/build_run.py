"""Run the production clock, status and barometer screens using real LVGL."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[2]
test = root / "tests/system_screens"
out = root / "build/host_system_screens"
out.mkdir(parents=True, exist_ok=True)
cache = Path(os.environ.get("AETHER_LVGL_CACHE", root / "build/host_reader_ui"))
lvgl = root / "managed_components/lvgl__lvgl"
cc = os.environ.get("CC", "cc")
objects = [p for p in sorted(cache.glob("managed_components_lvgl__lvgl_src_*.c.o"))
           if p.name != "managed_components_lvgl__lvgl_src_font_lv_font_montserrat_28.c.o"]
if not objects:
    raise SystemExit("Build the native LVGL cache with tests/reader_ui/build_run.py first")
objects += [cache / f"main_assets_font_{size}.c.o" for size in (16, 24)]

# The system file also owns unrelated Settings dependencies. Use its original
# status and clock functions unchanged, as volume_ui does for Settings.
source = (root / "main/apps/app_system.c").read_text(encoding="utf-8")
start = source.index("static lv_obj_t *action(")
end = source.index("\ntypedef struct {\n    lv_obj_t *volume,")
pages = out / "system_pages.c"
pages.write_text('#include "app_ui_internal.h"\n' + source[start:end], encoding="utf-8")
includes = [
    test / "stubs", root / "tests/battery_ui/stubs", root / "tests/player/stubs",
    root / "tests/reader_ui/stubs", root / "tests/reader_ui",
    root / "components/dps368/include", root / "components/pcf85063/include",
    root / "components/qmi8658a/include", root / "components/tg29/include",
    root / "main", root / "main/ui", lvgl,
]
flags = [
    cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
    "-ffunction-sections", "-fdata-sections", "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_CONF_SUPPRESS_DEFINE_CHECK=1",
    "-DLV_FONT_MONTSERRAT_28=1", *["-I" + str(p) for p in includes],
]
for path in (pages, root / "main/apps/app_sensors.c", root / "main/ui/app_battery.c",
             root / "main/app_tools_math.c", lvgl / "src/font/lv_font_montserrat_28.c", test / "smoke.c"):
    obj = out / (path.stem + ".o")
    subprocess.run([*flags, "-c", str(path), "-o", str(obj)], check=True)
    objects.append(obj)
rsp = out / "link.rsp"
rsp.write_text("".join(f'"{p.resolve().as_posix()}"\n' for p in objects), encoding="utf-8")
binary = out / ("system_screens.exe" if os.name == "nt" else "system_screens")
subprocess.run([cc, "-Wl,--gc-sections", "@" + str(rsp), "-lm", "-o", str(binary)], check=True)
subprocess.run([str(binary)], check=True)
