"""Own output directory; cached reader LVGL/font objects are read only."""
from pathlib import Path
import struct,subprocess,sys,zlib
root=Path(__file__).resolve().parents[2];test=root/'tests/files_ui';out=root/'build/host_files_ui';out.mkdir(parents=True,exist_ok=True)
lvgl=root/'managed_components/lvgl__lvgl';cached=root/'build/host_reader_ui'
objects=sorted(cached.glob('managed_components_lvgl__lvgl_src_*.c.o'))+[cached/'main_assets_font_16.c.o',cached/'main_assets_font_24.c.o']
flags=['gcc','-std=c11','-O0','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-DLV_CONF_INCLUDE_SIMPLE','-DLV_CONF_SUPPRESS_DEFINE_CHECK=1','-D_DEFAULT_SOURCE',*['-I'+str(p) for p in [test/'stubs',root/'tests/reader_ui/stubs',root/'tests/reader_ui',root/'main',root/'components/aether_storage/include',lvgl]]]
for src in [root/'main/apps/app_files.c',root/'main/app_text.c',root/'components/aether_storage/storage_probe.c',*[root/f'main/assets/icons/icon_{name}.c' for name in ['files','music','reader','video','nes']],test/'smoke.c']:
    obj=out/(src.stem+'.o');subprocess.run(flags+['-c',str(src),'-o',str(obj)],check=True);objects.append(obj)
binary=out/'files_ui';subprocess.run(['gcc','-Wl,--gc-sections',*map(str,objects),'-lm','-o',str(binary)],check=True);subprocess.run([str(binary),str(out),sys.argv[1] if len(sys.argv)>1 else 'after'],check=True)
for ppm in out.glob('*.ppm'):
    pixels=ppm.read_bytes().split(b'\n',3)[3]
    def chunk(t,p):return struct.pack('>I',len(p))+t+p+struct.pack('>I',zlib.crc32(t+p)&0xffffffff)
    scan=b''.join(b'\0'+pixels[y*1440:(y+1)*1440] for y in range(480));ppm.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',480,480,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))
