#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/app_audio_diagnostic.c"

static uint8_t master;
static ac101s_audio_output_t output;
static bool active, fail_alloc, fail_start, fail_read, fail_restore;
static unsigned writes, reads, stops, elapsed;
static int fail_write;
static int16_t last_pcm[960];
void *heap_caps_malloc(size_t bytes, unsigned caps)
{ assert(bytes==3840 && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)); return fail_alloc ? NULL : malloc(bytes); }
void vTaskDelay(TickType_t amount) { elapsed+=amount; }
esp_err_t ac101s_audio_get_master_volume(ac101s_audio_handle_t h,uint8_t *value)
{ assert(h); *value=master; return ESP_OK; }
esp_err_t ac101s_audio_get_output(ac101s_audio_handle_t h,ac101s_audio_output_t *value)
{ assert(h); *value=output; return ESP_OK; }
esp_err_t ac101s_audio_set_master_volume(ac101s_audio_handle_t h,uint8_t value)
{ assert(h); if(fail_restore && value==17) return ESP_FAIL; master=value; return ESP_OK; }
esp_err_t ac101s_audio_set_output(ac101s_audio_handle_t h,ac101s_audio_output_t value)
{ assert(h); output=value; return ESP_OK; }
esp_err_t ac101s_audio_start(ac101s_audio_handle_t h,int volume,float gain)
{ assert(h && !active && volume==100 && gain==0 && master==70 && output==AC101S_OUTPUT_SPEAKER); if(fail_start)return ESP_FAIL;active=true;return ESP_OK; }
esp_err_t ac101s_audio_stop(ac101s_audio_handle_t h)
{ assert(h && active);active=false;++stops;return ESP_OK; }
esp_err_t ac101s_audio_write(ac101s_audio_handle_t h,void *data,size_t bytes)
{
 assert(h && active && master==70 && bytes==sizeof(last_pcm));
 const int16_t *pcm=data;
 for(unsigned i=0;i<480;++i) { assert(pcm[i*2]==pcm[i*2+1]);assert(abs(pcm[i*2])<=12000); }
 memcpy(last_pcm,data,bytes); ++writes;
 return fail_write>0 && writes==(unsigned)fail_write ? ESP_FAIL : ESP_OK;
}
esp_err_t ac101s_audio_read(ac101s_audio_handle_t h,void *data,size_t bytes)
{assert(h && active && bytes==sizeof(last_pcm));++reads;if(fail_read)return ESP_FAIL;memcpy(data,last_pcm,bytes);return ESP_OK;}
const char *esp_err_to_name(esp_err_t error) { return error==ESP_OK ? "OK" : "FAIL"; }
static void reset(void)
{master=17;output=AC101S_OUTPUT_EXTERNAL;active=fail_alloc=fail_start=fail_read=fail_restore=false;writes=reads=stops=elapsed=0;fail_write=0;}
int main(void)
{
 reset(); assert(app_audio_diagnostic_run((void *)1)==ESP_OK);
 assert(writes==100 && reads==3 && stops==1 && !active && elapsed==100 && master==17 && output==AC101S_OUTPUT_EXTERNAL);
 reset();fail_alloc=true;assert(app_audio_diagnostic_run((void *)1)==ESP_ERR_NO_MEM && !writes && !active && master==17);
 reset();fail_start=true;assert(app_audio_diagnostic_run((void *)1)==ESP_FAIL && !writes && !stops && master==17 && output==AC101S_OUTPUT_EXTERNAL);
 reset();fail_write=54;assert(app_audio_diagnostic_run((void *)1)==ESP_FAIL && writes==54 && stops==1 && !active && master==17 && output==AC101S_OUTPUT_EXTERNAL);
 reset();fail_read=true;assert(app_audio_diagnostic_run((void *)1)==ESP_OK && writes==100 && reads==1 && !active && master==17);
 reset();fail_restore=true;assert(app_audio_diagnostic_run((void *)1)==ESP_FAIL && stops==1 && !active);
 puts("PASS boot speaker diagnostic: bounded stereo sine, existing RX feedback, no hidden player gain, OOM/start/write/read/restore faults and original route/volume restoration");
 return 0;
}
