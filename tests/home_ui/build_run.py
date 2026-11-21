"""Run the production launcher using real LVGL, project fonts and image assets."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import struct
import subprocess
import zlib

root=Path(__file__).resolve().parents[2]
test=root/'tests/home_ui'
out=root/'build/host_home_ui'
out.mkdir(parents=True,exist_ok=True)
cached=root/'build/host_reader_ui'
lvgl=root/'managed_components/lvgl__lvgl'
flags=['gcc','-std=c11','-O0','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',
       '-DLV_CONF_INCLUDE_SIMPLE','-DLV_LVGL_H_INCLUDE_SIMPLE','-D_DEFAULT_SOURCE',
       '-DLV_FONT_MONTSERRAT_20=1','-DLV_FONT_MONTSERRAT_28=1',
       *['-I'+str(p) for p in [test/'stubs',root/'tests/battery_ui/stubs',root/'tests/player/stubs',root/'tests/reader_ui/stubs',root/'tests/reader_ui',root/'main/ui',root/'main',root/'components/tg29/include',root/'components/pcf85063/include',lvgl]]]
objects=[]
rebuild=[]
for source in [*sorted((lvgl/'src').rglob('*.c')),root/'main/assets/font_16.c',root/'main/assets/font_24.c']:
    name=str(source.relative_to(root)).replace('/','_')+'.o'
    obj=cached/name
    if source.name in ['lv_font_montserrat_20.c','lv_font_montserrat_28.c'] or not obj.exists():
        obj=out/name
        rebuild.append((source,obj))
    objects.append(obj)
for source in [root/'main/ui/app_home.c',root/'main/ui/app_battery.c',*[p for p in sorted((root/'main/assets/icons').glob('icon_*.c')) if p.name != 'icon_compass.c'],test/'smoke.c']:
    obj=out/(source.stem+'.o')
    rebuild.append((source,obj))
    objects.append(obj)
def compile_pair(pair):
    source,obj=pair
    result=subprocess.run(flags+['-c',str(source),'-o',str(obj)],capture_output=True,text=True)
    if result.returncode:
        print(result.stderr)
        result.check_returncode()
with ThreadPoolExecutor(max_workers=8) as pool:list(pool.map(compile_pair,rebuild))
binary=out/'home_ui_smoke'
subprocess.run(['gcc','-Wl,--gc-sections',*map(str,objects),'-lm','-o',str(binary)],check=True)
subprocess.run([str(binary),str(out)],check=True)
for ppm in out.glob('*.ppm'):
    pixels=ppm.read_bytes().split(b'\n',3)[3]
    def chunk(t,p):return struct.pack('>I',len(p))+t+p+struct.pack('>I',zlib.crc32(t+p)&0xffffffff)
    scan=b''.join(b'\0'+pixels[y*480*3:(y+1)*480*3] for y in range(480))
    ppm.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',480,480,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))
