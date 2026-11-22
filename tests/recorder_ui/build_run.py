"""Own output directory; the existing reader LVGL/font cache is read only."""
from pathlib import Path
import os, struct, subprocess, zlib
root = Path(__file__).resolve().parents[2]
test = root / 'tests/recorder_ui'
out = root / 'build/host_recorder_ui'
out.mkdir(parents=True, exist_ok=True)
cached = Path(os.environ.get('AETHER_LVGL_CACHE', root / 'build/host_reader_ui'))
lvgl = root / 'managed_components/lvgl__lvgl'
objects = sorted(cached.glob('managed_components_lvgl__lvgl_src_*.c.o'))
objects += [cached / 'main_assets_font_16.c.o', cached / 'main_assets_font_24.c.o']
if not objects or any(not p.exists() for p in objects):
    raise SystemExit('Populate the native reader UI cache first')
flags = ['gcc', '-std=c11', '-O0', '-g', '-Wall', '-Wextra', '-Werror',
         '-ffunction-sections', '-fdata-sections', '-DLV_CONF_INCLUDE_SIMPLE',
         '-DLV_CONF_SUPPRESS_DEFINE_CHECK=1', '-DLV_FONT_MONTSERRAT_28=1', '-D_DEFAULT_SOURCE',
         *['-I'+str(p) for p in [test/'stubs', root/'tests/reader_ui/stubs',
            root/'tests/reader_ui', root/'main', root/'components/aether_recorder/include', lvgl]]]
# The shared cache leaves the optional heading size disabled; compile it locally.
objects = [p for p in objects if not p.name.endswith('lv_font_montserrat_28.c.o')]
for src in [root/'main/apps/app_recorder.c',
            lvgl/'src/font/lv_font_montserrat_28.c', test/'smoke.c']:
    obj = out / (src.stem+'.o')
    subprocess.run(flags+['-c',str(src),'-o',str(obj)],check=True)
    objects.append(obj)
binary=out/'recorder_ui'
subprocess.run(['gcc','-Wl,--gc-sections',*map(str,objects),'-lm','-o',str(binary)],check=True)
subprocess.run([str(binary),str(out)],check=True)
for ppm in out.glob('*.ppm'):
    pixels=ppm.read_bytes().split(b'\n',3)[3]
    def chunk(t,p): return struct.pack('>I',len(p))+t+p+struct.pack('>I',zlib.crc32(t+p)&0xffffffff)
    scan=b''.join(b'\0'+pixels[y*1440:(y+1)*1440] for y in range(480))
    ppm.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',480,480,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))
