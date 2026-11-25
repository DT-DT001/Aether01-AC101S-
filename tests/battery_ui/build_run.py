"""Production battery badge and exact common header, with read-only LVGL cache."""
from pathlib import Path
import os
import struct, subprocess, zlib
root=Path(__file__).resolve().parents[2]
test=root/'tests/battery_ui';out=root/'build/host_battery_ui';out.mkdir(parents=True,exist_ok=True)
cached=Path(os.environ.get('AETHER_LVGL_CACHE', root/'build/host_reader_ui'));lvgl=root/'managed_components/lvgl__lvgl'
compiler=os.environ.get('CC', 'gcc')
sanitizer='-fsanitize=undefined' if os.name == 'nt' else '-fsanitize=address,undefined'
objects=sorted(cached.glob('managed_components_lvgl__lvgl_src_*.c.o'))
objects += [cached/'main_assets_font_16.c.o',cached/'main_assets_font_24.c.o']
if not objects or any(not p.exists() for p in objects):raise SystemExit('Populate the native reader cache first')
objects=[p for p in objects if not p.name.endswith('lv_font_montserrat_20.c.o')]
ui=(root/'main/ui/app_ui.c').read_text(encoding='utf-8')
back=ui[ui.index('static bool navigate('):ui.index('static bool request_home(')]
back+=ui[ui.index('bool app_ui_go_back('):ui.index('void app_ui_set_user_key_exit(')]
header=ui[ui.index('lv_obj_t *app_ui_begin_page('):ui.index('static void shutdown_notice_close(')]
source=out/'common_header.c';source.write_text('#include "app_ui_internal.h"\n#include "app_battery.h"\n#include "app_screen.h"\n#define app_theme_background(color) (color)\n'+back+header,encoding='utf-8')
flags=[compiler,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',sanitizer,
       '-ffunction-sections','-fdata-sections','-DLV_CONF_INCLUDE_SIMPLE','-DLV_CONF_SUPPRESS_DEFINE_CHECK=1',
       '-DLV_FONT_MONTSERRAT_20=1','-D_DEFAULT_SOURCE',*['-I'+str(p) for p in [
       test/'stubs',root/'tests/reader_ui/stubs',root/'tests/reader_ui',root/'main/ui',root/'main',
       root/'components/tg29/include',root/'components/pcf85063/include',lvgl]]]
for src in [root/'main/ui/app_battery.c',source,lvgl/'src/font/lv_font_montserrat_20.c',test/'smoke.c']:
    obj=out/(src.stem+'.o');subprocess.run(flags+['-c',str(src),'-o',str(obj)],check=True);objects.append(obj)
binary=out/('battery_ui.exe' if os.name == 'nt' else 'battery_ui')
response=out/'battery-link.rsp'
response.write_text('\n'.join(f'"{obj.as_posix()}"' for obj in objects),encoding='utf-8')
subprocess.run([compiler,sanitizer,'-Wl,--gc-sections','@'+str(response),'-lm','-o',str(binary)],check=True)
subprocess.run([str(binary),str(out)],check=True,timeout=45)
for ppm in out.glob('*.ppm'):
    pixels=ppm.read_bytes().split(b'\n',3)[3]
    def chunk(t,p):return struct.pack('>I',len(p))+t+p+struct.pack('>I',zlib.crc32(t+p)&0xffffffff)
    scan=b''.join(b'\0'+pixels[y*1440:(y+1)*1440] for y in range(480))
    ppm.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',480,480,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))
