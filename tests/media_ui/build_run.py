"""Reuse reader_ui's native LVGL/font configuration; execute production media UI."""
from pathlib import Path
import subprocess
import struct
import zlib

root = Path(__file__).resolve().parents[2]
test = root / "tests/media_ui"
reader = root / "tests/reader_ui"
cached = root / "build/host_reader_ui"
out = root / "build/host_media_ui"
out.mkdir(parents=True, exist_ok=True)
lvgl = root / "managed_components/lvgl__lvgl"
includes = [test / "stubs", root / "components/aether_video_player/include", root / "components/aether_mp4_player/include", root / "components/aether_mjpeg_player/include",
            root / "components/aether_nes/include", root / "components/aether_audio_player/include",
            reader / "stubs", reader, root / "main", lvgl]
flags = ["gcc", "-std=c11", "-O0", "-g", "-ffunction-sections", "-fdata-sections",
         "-DLV_CONF_INCLUDE_SIMPLE", "-D_DEFAULT_SOURCE", *["-I" + str(p) for p in includes]]
objects = []
for source in [*sorted((lvgl / "src").rglob("*.c")), root / "main/assets/font_16.c",
               root / "main/assets/font_24.c", root / "main/app_text.c"]:
    name = str(source.relative_to(root)).replace("/", "_") + ".o"
    obj = cached / name
    if not obj.exists() or obj.stat().st_mtime < max(source.stat().st_mtime, (reader / "lv_conf.h").stat().st_mtime):
        obj = out / name
        subprocess.run(flags + ["-c", str(source), "-o", str(obj)], check=True)
    objects.append(obj)
for source in [root / "main/apps/app_media.c", root / "main/app_frames.c", test / "smoke.c"]:
    obj = out / (source.stem + ".o")
    subprocess.run(flags + ["-Wall", "-Wextra", "-Werror", "-c", str(source), "-o", str(obj)], check=True)
    objects.append(obj)
binary = out / "media_ui_smoke"
subprocess.run(["gcc", "-Wl,--gc-sections", *map(str, objects), "-lm", "-o", str(binary)], check=True)
subprocess.run([str(binary), str(out)], check=True)
def chunk(t,p):return struct.pack('>I',len(p))+t+p+struct.pack('>I',zlib.crc32(t+p)&0xffffffff)
for p in out.glob('fullscreen-*.ppm'):
    pixels=p.read_bytes().split(b'\n',3)[3]
    scan=b''.join(b'\0'+pixels[y*480*3:(y+1)*480*3] for y in range(480))
    p.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',480,480,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))
