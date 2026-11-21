"""Real LVGL keyboard/mouse processing against the production USB UI adapter."""
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[2]
lvgl = root / 'managed_components/lvgl__lvgl'
objects = sorted((root/'build/host_reader_ui').glob('managed_components_lvgl__lvgl_src_*.c.o'))
if not objects:
    raise SystemExit('Populate tests/reader_ui native LVGL cache first')
out = root/'build/host_usb_ui_input'
out.mkdir(parents=True, exist_ok=True)
binary = out/'usb_ui_input'
flags = ['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
         '-fsanitize=address,undefined','-ffunction-sections','-fdata-sections',
         '-DLV_CONF_INCLUDE_SIMPLE','-DLV_CONF_SUPPRESS_DEFINE_CHECK=1','-D_DEFAULT_SOURCE',
         *['-I'+str(p) for p in [root/'tests/audio/stubs',root/'tests/touch_lvgl/stubs',
           root/'tests/reader_ui',lvgl,root/'main',root/'components/aether_usb_host/include']]]
subprocess.run(flags+[str(root/'main/app_usb_input.c'),str(root/'tests/usb_ui_input/smoke.c'),
               *map(str,objects),'-Wl,--gc-sections','-lm','-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
