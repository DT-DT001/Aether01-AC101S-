"""Isolated sanitized queue and actual UI IO-completion core; no LVGL cache writes."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
test=root/'tests/ui_media_control'
out=root/'build/host_ui_media_control'
out.mkdir(parents=True,exist_ok=True)
source=(root/'main/ui/app_ui.c').read_text()
request=source[source.index('void app_ui_request_io_ex('):source.index('lv_obj_t *app_ui_add_text(')]
user_key=source[source.index('static void service_user_key('):source.index('lv_obj_t *app_ui_new_screen(')]
service=source[source.index('static void service_timer('):source.index('    bool measuring_media = false;')]
service=service.replace('static void service_timer(','void test_service_timer(',1)+'}\n'
core=out/'io_core.c';core.write_text('#include "app_ui_internal.h"\n'+request+user_key+service)
binary=out/'queue_test'
includes=[test/'stubs',root/'components/aether_recorder/include',root/'tests/player/stubs',root/'tests/bluetooth_runtime/stubs',root/'tests/bluetooth_ui/stubs',
          root/'tests/reader_ui/stubs',root/'main',root/'components/aether_bluetooth/include']
subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror',
    '-fsanitize=address,undefined','-pthread',*['-I'+str(p) for p in includes],
    str(root/'main/ui/app_ui_media_control.c'),str(core),str(test/'queue_test.c'),'-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
