from pathlib import Path
import sys
root=Path(__file__).resolve().parents[2]
s=(root/'main/main.c').read_text()
end=s.index('} shutdown_context_t;')+len('} shutdown_context_t;')
start=s.rfind('typedef struct {',0,end)
structure=s[start:end]
helpers=s[s.index('static void park_shutdown_peripherals'):s.index('static esp_err_t prepare_shutdown')]
start=s.index('if(stage==APP_POWER_SHUTDOWN_CANCEL) {')
brace=s.index('{',start);depth=1;end=brace+1
while depth:
    depth+=(s[end]=='{')-(s[end]=='}');end+=1
body=s[brace+1:end-1]
out=Path(sys.argv[1]);out.write_text(structure+'\n'+helpers+'\nstatic esp_err_t actual_cancel(void *context,uint32_t timeout_ms) {\nshutdown_context_t *state=context;\nconst int64_t deadline=esp_timer_get_time()+(int64_t)timeout_ms*1000;\naether_bluetooth_handle_t bluetooth=app_audio_system_bluetooth();\n'+body+'\n}\n')
