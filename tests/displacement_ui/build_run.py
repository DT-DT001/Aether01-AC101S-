"""Native LVGL regression for the short-distance measurement workflow."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
test = root / 'tests/displacement_ui'
out = root / 'build/host_displacement_ui'
out.mkdir(parents=True, exist_ok=True)
cached = root / 'build/host_reader_ui'
lvgl = root / 'managed_components/lvgl__lvgl'
objects = sorted(cached.glob('managed_components_lvgl__lvgl_src_*.c.o'))
if not objects:
    raise SystemExit('Populate the native reader LVGL cache first')
objects = [path for path in objects if not path.name.endswith('lv_font_montserrat_28.c.o')]
objects += [cached / 'main_assets_font_16.c.o', cached / 'main_assets_font_24.c.o']
includes = [root / 'tests/pc_remote_ui/stubs', root / 'tests/reader_ui/stubs',
            root / 'tests/reader_ui', root / 'main', lvgl]
flags = ['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections',
         '-DLV_CONF_INCLUDE_SIMPLE', '-DLV_CONF_SUPPRESS_DEFINE_CHECK=1',
         '-DLV_FONT_MONTSERRAT_28=1', '-D_DEFAULT_SOURCE', *['-I'+str(path) for path in includes]]
for source in [root / 'main/apps/app_displacement.c', lvgl / 'src/font/lv_font_montserrat_28.c', test / 'smoke.c']:
    obj = out / (source.stem + '.o')
    subprocess.run(flags + ['-c', str(source), '-o', str(obj)], check=True)
    objects.append(obj)
binary = out / 'displacement_ui'
subprocess.run(['gcc', '-fsanitize=address,undefined', '-Wl,--gc-sections',
                *map(str, objects), '-lm', '-o', str(binary)], check=True)
subprocess.run([str(binary), str(out)], check=True, timeout=30)
