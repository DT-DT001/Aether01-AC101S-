"""Real LVGL pointer gestures against production music and settings UI functions."""
from pathlib import Path
import os
import subprocess
root=Path(__file__).resolve().parents[2]
test=root/'tests/volume_ui'
out=root/'build/host_volume_ui'
out.mkdir(parents=True,exist_ok=True)
lvgl=root/'managed_components/lvgl__lvgl'
cached=Path(os.environ.get('AETHER_LVGL_CACHE', root/'build/host_reader_ui'))
compiler=os.environ.get('CC', 'gcc')
objects=sorted(cached.glob('managed_components_lvgl__lvgl_src_*.c.o'))
objects=[obj for obj in objects if not obj.name.endswith('_lv_font_montserrat_20.c.o')]
objects += [cached/'main_assets_font_16.c.o',cached/'main_assets_font_24.c.o']
if not objects:raise SystemExit('Run reader_ui build first')
# Keep the unchanged status/RTC pages out of this focused host harness. These
# settings functions and their shared action/free helpers are copied verbatim.
system=(root/'main/apps/app_system.c').read_text(encoding='utf-8')
helpers=system[system.index('static lv_obj_t *action('):system.index('static const char *component_status(')]
settings=system[system.index('typedef struct {\n    lv_obj_t *volume,'):]
source=out/'settings.c'
source.write_text('#include "app_ui_internal.h"\n#include "app_connection_style.h"\n#include "app_otg_control.h"\n#include "app_audio_system.h"\n#include "aether_bluetooth.h"\n#include "aether_wifi.h"\n#include "aether_usb_audio.h"\n#include "aether_usb_storage.h"\n#include "aether_media_fs.h"\n#include <stdlib.h>\n#include <stdio.h>\n'+helpers+settings, encoding='utf-8')
ui=(root/'main/ui/app_ui.c').read_text(encoding='utf-8')
shutdown=out/'shutdown.c'
shutdown.write_text('#include "app_ui_internal.h"\n#include "app_ui_modal.h"\n'+
    ui[ui.index('static void shutdown_notice_close('):ui.index('static void service_timer(')]+
    ui[ui.index('esp_err_t app_ui_quiesce_storage_begin('):ui.index('void app_ui_screen_suspend(')]+
    '\nvoid test_ui_shutdown_service(app_ui_t *ui) { app_ui_shutdown_service(ui); }\n', encoding='utf-8')
chrome=out/'page_chrome.c'
chrome.write_text('#include "app_ui_internal.h"\n#include "app_battery.h"\n#include "app_screen.h"\n'
    '#define app_theme_background(color) (color)\n'
    'lv_obj_t *app_ui_new_screen(app_ui_t *ui);\n'
    'void app_ui_set_user_key_exit(app_ui_t *ui, bool enabled);\n'
    'void app_ui_show_drawer(app_ui_t *ui);\n'+
    ui[ui.index('static bool navigate('):ui.index('static bool request_home(')]+
    ui[ui.index('bool app_ui_go_back('):ui.index('void app_ui_set_user_key_exit(')]+
    ui[ui.index('lv_obj_t *app_ui_begin_page('):ui.index('static void shutdown_notice_close(')], encoding='utf-8')
flags=[compiler,'-std=c11','-O0','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',
       '-DLV_CONF_INCLUDE_SIMPLE','-DLV_CONF_SUPPRESS_DEFINE_CHECK=1','-D_DEFAULT_SOURCE','-DAETHER_VOLUME_REAL_CHROME',
       '-DLV_FONT_MONTSERRAT_20=1',
       *['-I'+str(p) for p in [test/'stubs',root/'tests/reader_ui/stubs',root/'tests/home_ui/stubs',
          root/'tests/battery_ui/stubs',root/'tests/reader_ui',root/'main',root/'main/ui',root/'main/apps',
          root/'components/aether_wifi/include',
          root/'components/aether_usb_host/include',root/'components/aether_usb_audio/include',
          root/'components/aether_usb_storage/include',root/'components/tg29/include',
          root/'components/pcf85063/include',lvgl]]]
for src in [source,shutdown,chrome,lvgl/'src/font/lv_font_montserrat_20.c',root/'main/ui/app_battery.c',root/'main/ui/app_ui_modal.c',root/'main/apps/app_music.c',root/'main/app_lyrics.c',root/'main/app_text.c',root/'main/apps/app_usb.c',test/'smoke.c']:
    obj=out/(src.stem+'.o')
    subprocess.run(flags+['-c',str(src),'-o',str(obj)],check=True)
    objects.append(obj)
binary=out/('volume_ui.exe' if os.name == 'nt' else 'volume_ui')
response=out/'volume-link.rsp'
response.write_text('\n'.join(f'"{obj.as_posix()}"' for obj in objects),encoding='utf-8')
subprocess.run([compiler,'-Wl,--gc-sections','@'+str(response),'-lm','-o',str(binary)],check=True)
for offset in [-20,0,20]:subprocess.run([str(binary),str(offset),str(out)],check=True)
