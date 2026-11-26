"""Exercise production motion page/filter with real LVGL and supplied fonts."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import struct
import subprocess
import zlib

root=Path(__file__).resolve().parents[2]
test=root/'tests/motion_ui'
reader=root/'tests/reader_ui'
out=Path(os.environ.get('AETHER_MOTION_UI_OUT',root/'build/host_motion_ui')).resolve()
cached=Path(os.environ.get('AETHER_LVGL_CACHE',out if os.name=='nt' else root/'build/host_reader_ui'))
out.mkdir(parents=True,exist_ok=True)
lvgl=root/'managed_components/lvgl__lvgl'
includes=[test/'stubs',root/'components/qmi8658a/include',reader/'stubs',reader,
          root/'tests/pc_remote_ui/stubs',root/'main',root/'main/ui',lvgl]
cc=os.environ.get('CC','gcc')
flags=[cc,'-std=c11','-O1','-g','-ffunction-sections','-fdata-sections',
       '-DLV_CONF_INCLUDE_SIMPLE','-DLV_CONF_SUPPRESS_DEFINE_CHECK=1','-D_DEFAULT_SOURCE','-DLV_FONT_MONTSERRAT_28=1',
       *['-I'+str(p) for p in includes]]
def lvgl_object(source):
    obj=cached/(source.relative_to(root).as_posix().replace('/','_')+'.o')
    timestamp=max(source.stat().st_mtime,(reader/'lv_conf.h').stat().st_mtime)
    if source.name == 'lv_font_montserrat_28.c' or not obj.exists() or obj.stat().st_mtime<timestamp:
        obj=out/obj.name
        if not obj.exists() or obj.stat().st_mtime<timestamp:
            subprocess.run(flags+['-c',str(source),'-o',str(obj)],check=True)
    return obj

with ThreadPoolExecutor(max_workers=8) as pool:
    objects=list(pool.map(lvgl_object, [*sorted((lvgl/'src').rglob('*.c')),
                                      root/'main/assets/font_16.c',root/'main/assets/font_24.c']))
for source in [root/'main/apps/app_motion.c',root/'main/app_motion_filter.c',
               root/'main/ui/app_ui_gyro_calibration.c',root/'main/ui/app_ui_modal.c',test/'smoke.c']:
    obj=out/(source.stem+'.o')
    subprocess.run(flags+['-Wall','-Wextra','-Werror','-c',str(source),'-o',str(obj)],check=True)
    objects.append(obj)
binary=out/('motion_ui_smoke.exe' if os.name=='nt' else 'motion_ui_smoke')
response=out/'link.rsp'
response.write_text(''.join(f'"{p.resolve().as_posix()}"\n' for p in objects),encoding='utf-8')
subprocess.run([cc,'-Wl,--gc-sections','@'+str(response),'-lm','-o',str(binary)],check=True)
subprocess.run([str(binary),str(out)],check=True)
def chunk(kind,payload):
    return struct.pack('>I',len(payload))+kind+payload+struct.pack('>I',zlib.crc32(kind+payload)&0xffffffff)
for path in out.glob('*.ppm'):
    pixels=path.read_bytes().split(b'\n',3)[3]
    scan=b''.join(b'\0'+pixels[y*480*3:(y+1)*480*3] for y in range(480))
    path.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+
        chunk(b'IHDR',struct.pack('>IIBBBBB',480,480,8,2,0,0,0))+
        chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))
