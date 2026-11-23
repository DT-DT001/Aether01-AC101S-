"""Run the production arcade screens with real LVGL and an actual pointer driver."""

from pathlib import Path
import os
import shutil
import struct
import subprocess
import zlib

root = Path(__file__).resolve().parents[2]
test = root / "tests/game_ui"
out = root / "build/host_game_ui"
out.mkdir(parents=True, exist_ok=True)
lvgl = root / "managed_components/lvgl__lvgl"
windows = os.name == "nt"
cached = Path(os.environ.get("AETHER_LVGL_CACHE", str(root / (
    "build/host_win_lvgl" if windows else "build/host_reader_ui"))))
compiler = os.environ.get("CC", "clang" if windows else "cc")
objects = sorted(cached.glob("managed_components_lvgl__lvgl_src_*.c.o"))
if not objects:
    raise SystemExit(f"Build native LVGL objects first in {cached}")
objects += [cached / "main_assets_font_16.c.o", cached / "main_assets_font_24.c.o"]
flags = [
    "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
    "-ffunction-sections", "-fdata-sections", "-DLV_CONF_INCLUDE_SIMPLE",
    "-DLV_CONF_SUPPRESS_DEFINE_CHECK=1", "-DLV_FONT_MONTSERRAT_20=1", "-DLV_FONT_MONTSERRAT_28=1",
    *["-I" + str(path) for path in [test / "stubs", root / "tests/reader_ui", root / "tests/reader_ui/stubs",
                                    root / "tests/player/stubs", root / "main", lvgl]],
]
sanitizers = [] if windows else ["-fsanitize=address,undefined"]
sources = [
    root / "main/apps/app_game.c", root / "main/app_arcade.c",
    root / "main/app_motion_filter.c", test / "smoke.c",
]
if not windows:
    fonts = {"lv_font_montserrat_20.c", "lv_font_montserrat_28.c"}
    objects = [obj for obj in objects if not any(obj.name.endswith(font + ".o") for font in fonts)]
    sources += [lvgl / "src/font" / font for font in sorted(fonts)]
for source in sources:
    obj = out / (source.stem + ".o")
    subprocess.run([compiler, *flags, *sanitizers, "-c", str(source), "-o", str(obj)], check=True)
    objects.append(obj)
binary = out / ("game_ui.exe" if windows else "game_ui")
response = out / "objects.rsp"
response.write_text("\n".join(f'"{obj.as_posix()}"' for obj in objects), encoding="utf-8")
subprocess.run([compiler, *sanitizers, "-Wl,--gc-sections", f"@{response}", "-lm", "-o", str(binary)], check=True)
env = dict(os.environ)
if windows:
    executable = shutil.which(compiler)
    if executable:
        env["PATH"] = str(Path(executable).parent) + os.pathsep + env["PATH"]
subprocess.run([str(binary), str(out)], check=True, timeout=45, env=env)


def png_chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))


for ppm in out.glob("*.ppm"):
    pixels = ppm.read_bytes().split(b"\n", 3)[3]
    stride = 480 * 3
    scanlines = b"".join(b"\0" + pixels[offset:offset + stride] for offset in range(0, len(pixels), stride))
    header = struct.pack(">IIBBBBB", 480, 480, 8, 2, 0, 0, 0)
    ppm.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" +
                                       png_chunk(b"IHDR", header) +
                                       png_chunk(b"IDAT", zlib.compress(scanlines)) +
                                       png_chunk(b"IEND", b""))
