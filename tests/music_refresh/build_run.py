"""Production music UI with controlled asynchronous catalog completion."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
lvgl=root/'managed_components/lvgl__lvgl'
cached=root/'build/host_reader_ui'
objects=sorted(cached.glob('managed_components_lvgl__lvgl_src_*.c.o'))
objects += [cached/'main_assets_font_16.c.o',cached/'main_assets_font_24.c.o']
if not objects: raise SystemExit('Populate native reader UI cache first')
out=root/'build/host_music_refresh';out.mkdir(parents=True,exist_ok=True)
binary=out/'music_refresh'
flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
       '-fsanitize=address,undefined','-ffunction-sections','-fdata-sections',
       '-DLV_CONF_INCLUDE_SIMPLE','-DLV_CONF_SUPPRESS_DEFINE_CHECK=1','-D_DEFAULT_SOURCE',
       *['-I'+str(p) for p in [root/'tests/volume_ui/stubs',root/'tests/reader_ui/stubs',
         root/'tests/home_ui/stubs',root/'tests/reader_ui',root/'main',root/'main/ui',
         root/'components/aether_usb_host/include',root/'components/aether_usb_audio/include',
         root/'components/aether_usb_storage/include',lvgl]]]
subprocess.run(flags+[str(root/'main/apps/app_music.c'),str(root/'tests/music_refresh/smoke.c'),
               str(root/'main/app_text.c'),
               *map(str,objects),'-Wl,--gc-sections','-lm','-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
