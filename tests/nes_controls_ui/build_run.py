from pathlib import Path
import subprocess,struct,zlib
root=Path(__file__).resolve().parents[2];test=root/'tests/nes_controls_ui';out=root/'build/host_nes_controls_ui';out.mkdir(parents=True,exist_ok=True)
lvgl=root/'managed_components/lvgl__lvgl';cached=root/'build/host_reader_ui'
objects=sorted(cached.glob('managed_components_lvgl__lvgl_src_*.c.o'))+[cached/'main_assets_font_16.c.o',cached/'main_assets_font_24.c.o']
flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-pthread','-ffunction-sections','-fdata-sections','-DLV_CONF_INCLUDE_SIMPLE','-DLV_CONF_SUPPRESS_DEFINE_CHECK=1','-D_DEFAULT_SOURCE',*['-I'+str(p) for p in [test/'stubs',root/'tests/nes_controls/stubs',root/'tests/reader_ui/stubs',root/'tests/reader_ui',root/'tests/audio/stubs',root/'main',lvgl]]]
for src in [root/'main/apps/app_controls.c',root/'main/ui/app_ui_modal.c',test/'smoke.c']:
 obj=out/(src.stem+'.o');subprocess.run(flags+['-c',str(src),'-o',str(obj)],check=True);objects.append(obj)
binary=out/'nes_controls_ui';subprocess.run(['gcc','-fsanitize=address,undefined','-pthread','-Wl,--gc-sections',*map(str,objects),'-lm','-o',str(binary)],check=True);subprocess.run([str(binary),str(out)],check=True)
for ppm in out.glob('*.ppm'):
 pixels=ppm.read_bytes().split(b'\n',3)[3]
 def chunk(t,p):return struct.pack('>I',len(p))+t+p+struct.pack('>I',zlib.crc32(t+p)&0xffffffff)
 scan=b''.join(b'\0'+pixels[y*1440:(y+1)*1440] for y in range(480));ppm.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',480,480,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))
