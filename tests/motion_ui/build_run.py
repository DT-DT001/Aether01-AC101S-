"""Exercise production motion page/filter with real LVGL and supplied fonts."""
from pathlib import Path
import struct
import subprocess
import zlib

root=Path(__file__).resolve().parents[2]
test=root/'tests/motion_ui'
reader=root/'tests/reader_ui'
cached=root/'build/host_reader_ui'
out=root/'build/host_motion_ui'
out.mkdir(parents=True,exist_ok=True)
lvgl=root/'managed_components/lvgl__lvgl'
includes=[test/'stubs',root/'components/qmi8658a/include',reader/'stubs',reader,root/'main',lvgl]
flags=['gcc','-std=c11','-O0','-g','-ffunction-sections','-fdata-sections',
       '-DLV_CONF_INCLUDE_SIMPLE','-DLV_CONF_SUPPRESS_DEFINE_CHECK=1','-D_DEFAULT_SOURCE',
       *['-I'+str(p) for p in includes]]
objects=[]
for source in [*sorted((lvgl/'src').rglob('*.c')),root/'main/assets/font_16.c',root/'main/assets/font_24.c']:
    obj=cached/(str(source.relative_to(root)).replace('/','_')+'.o')
    if not obj.exists() or obj.stat().st_mtime<max(source.stat().st_mtime,(reader/'lv_conf.h').stat().st_mtime):
        obj=out/obj.name
        subprocess.run(flags+['-c',str(source),'-o',str(obj)],check=True)
    objects.append(obj)
for source in [root/'main/apps/app_motion.c',root/'main/app_motion_filter.c',test/'smoke.c']:
    obj=out/(source.stem+'.o')
    subprocess.run(flags+['-Wall','-Wextra','-Werror','-c',str(source),'-o',str(obj)],check=True)
    objects.append(obj)
binary=out/'motion_ui_smoke'
subprocess.run(['gcc','-Wl,--gc-sections',*map(str,objects),'-lm','-o',str(binary)],check=True)
subprocess.run([str(binary),str(out)],check=True)
def chunk(kind,payload):
    return struct.pack('>I',len(payload))+kind+payload+struct.pack('>I',zlib.crc32(kind+payload)&0xffffffff)
for path in out.glob('*.ppm'):
    pixels=path.read_bytes().split(b'\n',3)[3]
    scan=b''.join(b'\0'+pixels[y*480*3:(y+1)*480*3] for y in range(480))
    path.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+
        chunk(b'IHDR',struct.pack('>IIBBBBB',480,480,8,2,0,0,0))+
        chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))
