"""Exercise the production GNSS page with native LVGL and mocked UART/power."""
from pathlib import Path
import struct
import subprocess
import zlib

root = Path(__file__).resolve().parents[2]
test = root / 'tests/gps_ui'
out = root / 'build/host_gps_ui'
out.mkdir(parents=True, exist_ok=True)
cached = root / 'build/host_reader_ui'
if not cached.exists():
    cached = root / 'build/host_home_ui'
lvgl = root / 'managed_components/lvgl__lvgl'
objects = sorted(cached.glob('managed_components_lvgl__lvgl_src_*.c.o'))
if not objects:
    raise SystemExit('Run tests/home_ui/build_run.py first to populate the native LVGL cache')
objects += [cached / 'main_assets_font_16.c.o', cached / 'main_assets_font_24.c.o']
includes = [root / 'tests/pc_remote_ui/stubs', root / 'tests/reader_ui/stubs',
            root / 'tests/reader_ui', root / 'main',
            root / 'components/aether_gnss/include', lvgl]
flags = ['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections',
         '-DLV_CONF_INCLUDE_SIMPLE', '-DLV_CONF_SUPPRESS_DEFINE_CHECK=1',
         '-D_DEFAULT_SOURCE', *['-I'+str(path) for path in includes]]
for source in [root / 'main/apps/app_gps.c', test / 'smoke.c']:
    obj = out / (source.stem + '.o')
    subprocess.run(flags + ['-c', str(source), '-o', str(obj)], check=True)
    objects.append(obj)
binary = out / 'gps_ui'
subprocess.run(['gcc', '-fsanitize=address,undefined', '-Wl,--gc-sections',
                *map(str, objects), '-lm', '-o', str(binary)], check=True)
subprocess.run([str(binary), str(out)], check=True, timeout=45)
for ppm in out.glob('*.ppm'):
    pixels = ppm.read_bytes().split(b'\n', 3)[3]
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    rows = b''.join(b'\0' + pixels[y*480*3:(y+1)*480*3] for y in range(480))
    ppm.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB', 480, 480, 8, 2, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
