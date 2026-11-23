"""Real LVGL controls, production pages/modal, and a deterministic HID double."""
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

root = Path(__file__).resolve().parents[2]
test = root / 'tests/pc_remote_ui'
out = root / 'build/host_pc_remote_ui'
out.mkdir(parents=True, exist_ok=True)
cached = root / 'build/host_reader_ui'
lvgl = root / 'managed_components/lvgl__lvgl'
objects = sorted(cached.glob('managed_components_lvgl__lvgl_src_*.c.o'))
objects += [cached / 'main_assets_font_16.c.o', cached / 'main_assets_font_24.c.o']
if not objects or any(not path.exists() for path in objects):
    raise SystemExit('Populate the native reader LVGL cache first')
objects = [path for path in objects if not path.name.endswith(('lv_font_montserrat_20.c.o', 'lv_font_montserrat_28.c.o'))]
includes = [test / 'stubs', root / 'tests/bluetooth_ui/stubs', root / 'tests/reader_ui/stubs',
            root / 'tests/utilities/ui_stubs', root / 'tests/reader_ui', root / 'main', root / 'main/ui',
            root / 'components/aether_bluetooth/include', lvgl]
flags = ['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections',
         '-DLV_CONF_INCLUDE_SIMPLE', '-DLV_CONF_SUPPRESS_DEFINE_CHECK=1',
         '-DLV_FONT_MONTSERRAT_20=1', '-DLV_FONT_MONTSERRAT_28=1', '-D_DEFAULT_SOURCE',
         *['-I'+str(path) for path in includes]]
keyboard = out / 'app_text_input.c'
shutil.copyfile(root / 'main/ui/app_text_input.c', keyboard)
for source in [root / 'main/apps/app_pc_remote.c', root / 'main/ui/app_ui_modal.c',
               root / 'main/ui/app_ui_gyro_calibration.c',
               keyboard, root / 'main/app_pinyin.c', root / 'main/app_pinyin_swipe.c',
               root / 'main/app_text.c', lvgl / 'src/font/lv_font_montserrat_20.c',
               lvgl / 'src/font/lv_font_montserrat_28.c', test / 'smoke.c']:
    obj = out / (source.stem + '.o')
    source_flags = ['-O0'] if source == keyboard else []
    subprocess.run(flags + source_flags + ['-c', str(source), '-o', str(obj)], check=True)
    objects.append(obj)
binary = out / 'pc_remote_ui'
subprocess.run(['gcc', '-fsanitize=address,undefined', '-Wl,--gc-sections',
                *map(str, objects), '-lm', '-o', str(binary)], check=True)
subprocess.run([str(binary), str(out)], check=True, timeout=45)

def chunk(kind, payload):
    return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))

for ppm in out.glob('*.ppm'):
    pixels = ppm.read_bytes().split(b'\n', 3)[3]
    scan = b''.join(b'\0' + pixels[y*1440:(y+1)*1440] for y in range(480))
    header = struct.pack('>IIBBBBB', 480, 480, 8, 2, 0, 0, 0)
    ppm.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
                                        chunk(b'IDAT', zlib.compress(scan)) + chunk(b'IEND', b''))
