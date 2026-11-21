"""Production battery badge and exact common header, with read-only LVGL cache."""
from pathlib import Path
import struct, subprocess, zlib
root=Path(__file__).resolve().parents[2]
test=root/'tests/battery_ui';out=root/'build/host_battery_ui';out.mkdir(parents=True,exist_ok=True)
cached=root/'build/host_reader_ui';lvgl=root/'managed_components/lvgl__lvgl'
objects=sorted(cached.glob('managed_components_lvgl__lvgl_src_*.c.o'))
objects += [cached/'main_assets_font_16.c.o',cached/'main_assets_font_24.c.o']
if not objects or any(not p.exists() for p in objects):raise SystemExit('Populate the native reader cache first')
objects=[p for p in objects if not p.name.endswith('lv_font_montserrat_20.c.o')]
ui=(root/'main/ui/app_ui.c').read_text()
back=ui[ui.index('static void back_event_cb('):ui.index('lv_obj_t *app_ui_new_screen(')]
header=ui[ui.index('lv_obj_t *app_ui_begin_page('):ui.index('static void service_timer(')]
source=out/'common_header.c';source.write_text('#include "app_ui_internal.h"\n#include "app_battery.h"\n'+back+header)
flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
       '-ffunction-sections','-fdata-sections','-DLV_CONF_INCLUDE_SIMPLE','-DLV_CONF_SUPPRESS_DEFINE_CHECK=1',
       '-DLV_FONT_MONTSERRAT_20=1','-D_DEFAULT_SOURCE',*['-I'+str(p) for p in [
       test/'stubs',root/'tests/reader_ui/stubs',root/'tests/reader_ui',root/'main/ui',root/'main',
       root/'components/tg29/include',root/'components/pcf85063/include',lvgl]]]
for src in [root/'main/ui/app_battery.c',source,lvgl/'src/font/lv_font_montserrat_20.c',test/'smoke.c']:
    obj=out/(src.stem+'.o');subprocess.run(flags+['-c',str(src),'-o',str(obj)],check=True);objects.append(obj)
binary=out/'battery_ui'
subprocess.run(['gcc','-fsanitize=address,undefined','-Wl,--gc-sections',*map(str,objects),'-lm','-o',str(binary)],check=True)
subprocess.run([str(binary),str(out)],check=True,timeout=45)
for ppm in out.glob('*.ppm'):
    pixels=ppm.read_bytes().split(b'\n',3)[3]
    def chunk(t,p):return struct.pack('>I',len(p))+t+p+struct.pack('>I',zlib.crc32(t+p)&0xffffffff)
    scan=b''.join(b'\0'+pixels[y*1440:(y+1)*1440] for y in range(480))
    ppm.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',480,480,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))
