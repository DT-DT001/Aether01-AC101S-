"""Test the production AC101S Gallery using native LVGL and controlled storage."""
from pathlib import Path
import os
import struct
import subprocess

root = Path(__file__).resolve().parents[2]
test = root / "tests/gallery_ui"
out = root / "build/host_gallery_ui"
out.mkdir(parents=True, exist_ok=True)
cache = Path(os.environ.get("AETHER_LVGL_CACHE", root / "build/host_reader_ui"))
cc = os.environ.get("CC", "cc")
objects = sorted(cache.glob("managed_components_lvgl__lvgl_src_*.c.o"))
if not objects:
    raise SystemExit("Build native LVGL with tests/reader_ui/build_run.py first")
objects += [cache / f"main_assets_font_{size}.c.o" for size in (16, 24)]
lvgl = root / "managed_components/lvgl__lvgl"
includes = [test / "stubs", root / "tests/reader_ui/stubs",
            root / "tests/player/stubs", root / "components/aether_image/include",
            root / "components/aether_storage/include",
            root / "tests/reader_ui", root / "main", root / "main/ui", lvgl]
flags = [cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-ffunction-sections",
         "-fdata-sections", "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_CONF_SUPPRESS_DEFINE_CHECK=1",
         "-DLV_FONT_MONTSERRAT_20=1", "-DLV_FONT_MONTSERRAT_48=1",
         *["-I" + str(p) for p in includes]]
binary = out / ("gallery_ui.exe" if os.name == "nt" else "gallery_ui")
rsp = out / "link.rsp"
rsp.write_text("".join(f'"{p.resolve().as_posix()}"\n' for p in objects), encoding="utf-8")
sources = [test / "smoke.c", test / "appearance_flash.c", root / "main/app_wallpaper_store.c",
           root / "main/app_text.c", lvgl / "src/font/lv_font_montserrat_20.c",
           lvgl / "src/font/lv_font_montserrat_48.c"]
subprocess.run([*flags, *map(str, sources), "@" + str(rsp),
                "-Wl,--gc-sections", "-lm", "-o", str(binary)], check=True)
photo_dir = out / "photos"
photo_dir.mkdir(exist_ok=True)
pixels = bytes([0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0])
header = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
dib = struct.pack("<IiiHHIIiiII", 40, 2, 2, 1, 24, 0, len(pixels), 0, 0, 0, 0)
(photo_dir / "tail.bmp").write_bytes(header + dib + pixels)
width, height = 600, 960
portrait = bytes(channel for y in range(height) for x in range(width)
                 for channel in (96 + y * 132 // height, x * 188 // width, 255 - y * 160 // height))
header_rgb = struct.pack("<2sIHHI", b"BM", 54 + len(portrait), 0, 0, 54)
portrait_dib = struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 24, 0, len(portrait), 0, 0, 0, 0)
(photo_dir / "portrait.bmp").write_bytes(header_rgb + portrait_dib + portrait)

colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255), (0, 0, 0), (0, 255, 255)]
for name, width, height, top_down in [("wallpaper-wide", 3, 2, False),
                                     ("wallpaper-tall", 2, 3, True),
                                     ("wallpaper-tiny", 1, 1, False)]:
    rows = []
    for y in range(height):
        row = bytes(channel for red, green, blue in colors[y * width:(y + 1) * width]
                    for channel in (blue, green, red))
        rows.append(row.ljust((len(row) + 3) & ~3, b"\0"))
    data = b"".join(rows if top_down else rows[::-1])
    header = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, width, -height if top_down else height,
                      1, 24, 0, len(data), 0, 0, 0, 0)
    (photo_dir / f"{name}.bmp").write_bytes(header + dib + data)
usb_dir = out / "usbphotos/DCIM/Camera"
usb_dir.mkdir(parents=True, exist_ok=True)
(usb_dir / "tail.bmp").write_bytes((photo_dir / "tail.bmp").read_bytes())
subprocess.run([str(binary)], check=True, cwd=out)
