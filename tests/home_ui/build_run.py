"""Run the production launcher using real LVGL, project fonts and image assets."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import struct
import subprocess
import zlib

root=Path(__file__).resolve().parents[2]
test=root/'tests/home_ui'
out=root/'build/host_home_ui'
out.mkdir(parents=True,exist_ok=True)
cached=Path(os.environ.get('AETHER_LVGL_CACHE', root/'build/host_reader_ui'))
if not cached.exists():
    cached=out
compiler=os.environ.get('CC', 'gcc')
lvgl=root/'managed_components/lvgl__lvgl'
flags=[compiler,'-std=c11','-O0','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',
       '-DLV_CONF_INCLUDE_SIMPLE','-DLV_LVGL_H_INCLUDE_SIMPLE','-D_DEFAULT_SOURCE',
       '-DLV_FONT_MONTSERRAT_20=1','-DLV_FONT_MONTSERRAT_28=1','-DLV_FONT_MONTSERRAT_48=1',
       '-include',str(test/'stubs/aether_bluetooth.h'),
       *['-I'+str(p) for p in [test/'stubs',root/'tests/battery_ui/stubs',root/'tests/player/stubs',root/'tests/reader_ui/stubs',root/'tests/reader_ui',root/'main/ui',root/'main',root/'components/aether_wifi/include',root/'components/tg29/include',root/'components/pcf85063/include',lvgl]]]
objects=[]
rebuild=[]
lvgl_sources=[p for p in sorted((lvgl/'src').rglob('*.c'))
              if not any(disabled in p.as_posix() for disabled in ['drivers/opengles/', 'libs/gltf/'])]
for source in [*lvgl_sources,root/'main/assets/font_16.c',root/'main/assets/font_24.c']:
    name=source.relative_to(root).as_posix().replace('/','_')+'.o'
    obj=cached/name
    if (source.name in ['lv_font_montserrat_20.c','lv_font_montserrat_28.c','lv_font_montserrat_48.c'] or not obj.exists() or obj.stat().st_mtime <
            max(source.stat().st_mtime,(root/'tests/reader_ui/lv_conf.h').stat().st_mtime)):
        obj=out/name
        rebuild.append((source,obj))
    objects.append(obj)
for source in [root/'main/ui/app_home.c',root/'main/ui/app_battery.c',*[p for p in sorted((root/'main/assets/icons').glob('icon_*.c')) if p.name != 'icon_compass.c'],test/'smoke.c']:
    obj=out/(source.stem+'.o')
    rebuild.append((source,obj))
    objects.append(obj)
def compile_pair(pair):
    source,obj=pair
    upstream_flags=['-Wno-unused-parameter'] if lvgl in source.parents else []
    result=subprocess.run(flags+upstream_flags+['-c',str(source),'-o',str(obj)],capture_output=True,text=True)
    if result.returncode:
        print(result.stderr)
        result.check_returncode()
with ThreadPoolExecutor(max_workers=8) as pool:list(pool.map(compile_pair,rebuild))
binary=out/('home_ui_smoke.exe' if os.name == 'nt' else 'home_ui_smoke')
response=out/'home-link.rsp'
response.write_text('\n'.join(f'"{obj.as_posix()}"' for obj in objects), encoding='utf-8')
subprocess.run([compiler,'-Wl,--gc-sections','@'+str(response),'-lm','-o',str(binary)],check=True)
subprocess.run([str(binary),str(out)],check=True)
for ppm in out.glob('*.ppm'):
    pixels=ppm.read_bytes().split(b'\n',3)[3]
    def chunk(t,p):return struct.pack('>I',len(p))+t+p+struct.pack('>I',zlib.crc32(t+p)&0xffffffff)
    scan=b''.join(b'\0'+pixels[y*480*3:(y+1)*480*3] for y in range(480))
    ppm.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',480,480,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))
