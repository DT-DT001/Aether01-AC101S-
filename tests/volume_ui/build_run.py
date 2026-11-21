"""Real LVGL pointer gestures against production music and settings UI functions."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
test=root/'tests/volume_ui'
out=root/'build/host_volume_ui'
out.mkdir(parents=True,exist_ok=True)
lvgl=root/'managed_components/lvgl__lvgl'
objects=sorted((root/'build/host_reader_ui').glob('managed_components_lvgl__lvgl_src_*.c.o'))
objects += [root/'build/host_reader_ui/main_assets_font_16.c.o',root/'build/host_reader_ui/main_assets_font_24.c.o']
if not objects:raise SystemExit('Run reader_ui build first')
# Keep the unchanged status/RTC pages out of this focused host harness. These
# settings functions and their shared action/free helpers are copied verbatim.
system=(root/'main/apps/app_system.c').read_text()
helpers=system[system.index('static lv_obj_t *action('):system.index('static void update_status(')]
settings=system[system.index('typedef struct {\n    lv_obj_t *volume,'):]
source=out/'settings.c'
source.write_text('#include "app_ui_internal.h"\n#include "app_otg_control.h"\n#include "app_audio_system.h"\n#include "aether_usb_audio.h"\n#include "aether_usb_storage.h"\n#include "aether_media_fs.h"\n#include <stdlib.h>\n#include <stdio.h>\n'+helpers+settings)
ui=(root/'main/ui/app_ui.c').read_text()
shutdown=out/'shutdown.c'
shutdown.write_text('#include "app_ui_internal.h"\n#include "app_ui_modal.h"\n'+
    ui[ui.index('static void shutdown_notice_close('):ui.index('static void service_timer(')]+
    ui[ui.index('esp_err_t app_ui_quiesce_storage_begin('):ui.index('esp_err_t app_ui_start(')]+
    '\nvoid test_ui_shutdown_service(app_ui_t *ui) { app_ui_shutdown_service(ui); }\n')
flags=['gcc','-std=c11','-O0','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',
       '-DLV_CONF_INCLUDE_SIMPLE','-DLV_CONF_SUPPRESS_DEFINE_CHECK=1','-D_DEFAULT_SOURCE',
       *['-I'+str(p) for p in [test/'stubs',root/'tests/reader_ui/stubs',root/'tests/home_ui/stubs',root/'tests/reader_ui',root/'main',root/'main/ui',root/'components/aether_usb_host/include',root/'components/aether_usb_audio/include',root/'components/aether_usb_storage/include',lvgl]]]
for src in [source,shutdown,root/'main/ui/app_ui_modal.c',root/'main/apps/app_music.c',root/'main/apps/app_usb.c',test/'smoke.c']:
    obj=out/(src.stem+'.o')
    subprocess.run(flags+['-c',str(src),'-o',str(obj)],check=True)
    objects.append(obj)
binary=out/'volume_ui'
subprocess.run(['gcc','-Wl,--gc-sections',*map(str,objects),'-lm','-o',str(binary)],check=True)
for offset in [-20,0,20]:subprocess.run([str(binary),str(offset),str(out)],check=True)
