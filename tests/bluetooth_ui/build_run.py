"""Compile the real Bluetooth page and drive actual LVGL pointer input."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
test=root/'tests/bluetooth_ui'
out=root/'build/host_bluetooth_ui'
out.mkdir(parents=True,exist_ok=True)
lvgl=root/'managed_components/lvgl__lvgl'
objects=sorted((root/'build/host_reader_ui').glob('managed_components_lvgl__lvgl_src_*.c.o'))
objects += [root/'build/host_reader_ui/main_assets_font_16.c.o',root/'build/host_reader_ui/main_assets_font_24.c.o']
flags=['gcc','-std=c11','-O0','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',
       '-DLV_CONF_INCLUDE_SIMPLE','-DLV_CONF_SUPPRESS_DEFINE_CHECK=1','-D_DEFAULT_SOURCE',
       *['-I'+str(p) for p in [test/'stubs',root/'tests/reader_ui/stubs',root/'tests/reader_ui',root/'main',root/'components/aether_bluetooth/include',lvgl]]]
for src in [root/'main/apps/app_bluetooth.c',test/'smoke.c']:
    obj=out/(src.stem+'.o');subprocess.run(flags+['-c',str(src),'-o',str(obj)],check=True);objects.append(obj)
binary=out/'bluetooth_ui';subprocess.run(['gcc','-Wl,--gc-sections',*map(str,objects),'-lm','-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
