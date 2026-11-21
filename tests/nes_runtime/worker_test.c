/* Full production worker/control lifecycle with recoverable teardown faults. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stubs/runtime.h"
#include "../../components/aether_nes/aether_nes.c"
static void (*worker)(void *);static void *worker_arg;
static unsigned ticks,leases,starts,stops,port_stops;
static esp_err_t audio_stop_error,port_stop_error;
static bool run_return_stop=true,codec_open,save_failure;
static const char *fixture;
size_t strlcpy(char *d,const char *s,size_t n){size_t len=strlen(s);if(n){size_t k=len<n-1?len:n-1;memcpy(d,s,k);d[k]=0;}return len;}
void *heap_caps_malloc(size_t n,unsigned c){(void)c;return malloc(n);}
const char *esp_err_to_name(esp_err_t e){(void)e;return "error";}
bool aether_nes_port_mapper_supported(unsigned m){return m==0;}
bool aether_storage_card_inserted(aether_storage_handle_t h){assert(h==(void*)1);return true;}
esp_err_t aether_storage_acquire(aether_storage_handle_t h,uint32_t t){assert(h==(void*)1&&t==0);++leases;return ESP_OK;}
void aether_storage_release(aether_storage_handle_t h){assert(h==(void*)1&&leases);--leases;}
esp_err_t ac101s_audio_start(ac101s_audio_handle_t h,int v,float g){assert(h==(void*)2&&v==65&&g==0);assert(!codec_open);++starts;codec_open=true;return ESP_OK;}
esp_err_t ac101s_audio_stop(ac101s_audio_handle_t h){assert(h==(void*)2);++stops;if(audio_stop_error)return audio_stop_error;codec_open=false;return ESP_OK;}
void aether_nes_port_set_active(aether_nes_handle_t h){assert(h==worker_arg);}
int aether_nes_port_run(void){aether_nes_handle_t h=worker_arg;if(save_failure){h->persistence_error=ESP_FAIL;strlcpy(h->detail,"save failed; old backup retained",sizeof(h->detail));}if(run_return_stop)atomic_store(&h->stop_requested,true);return 0;}
esp_err_t aether_nes_port_shutdown(void){++port_stops;return port_stop_error;}
int xTaskCreate(void(*fn)(void*),const char*n,unsigned stack,void*a,unsigned pri,void*out){assert(!strcmp(n,"nes")&&stack==12288&&pri==7&&!out);worker=fn;worker_arg=a;return pdPASS;}
void vTaskDelete(void *t){assert(!t);}
void vTaskDelay(uint32_t t){ticks+=t;}
TickType_t xTaskGetTickCount(void){return ticks;}
static void frame(const uint16_t *p,uint16_t w,uint16_t h,uint32_t f,void*c){(void)p;(void)w;(void)h;(void)f;(void)c;}
static aether_nes_handle_t new_player(void){aether_nes_handle_t h=NULL;assert(aether_nes_new((void*)1,(void*)2,(void*)3,&h)==ESP_OK);return h;}
static void start(aether_nes_handle_t h){assert(aether_nes_start(h,fixture,frame,NULL)==ESP_OK);assert(aether_nes_set_external_input(h,AETHER_NES_INPUT_A|AETHER_NES_INPUT_UP)==ESP_OK);aether_nes_status_t s;assert(aether_nes_get_status(h,&s)==ESP_OK&&s.external_buttons==(AETHER_NES_INPUT_A|AETHER_NES_INPUT_UP));worker(worker_arg);assert(!h->running&&!leases);}
int main(int argc,char**argv){
 assert(argc==2);fixture=argv[1];FILE*f=fopen(fixture,"wb");assert(f);uint8_t*rom=calloc(1,16+16384);assert(rom);memcpy(rom,"NES\x1a",4);rom[4]=1;assert(fwrite(rom,1,16+16384,f)==16+16384);fclose(f);free(rom);
 aether_nes_handle_t h=new_player();
 assert(aether_nes_set_external_input(h,0)==ESP_OK);assert(aether_nes_set_external_input(h,AETHER_NES_INPUT_A)==ESP_ERR_INVALID_STATE);assert(aether_nes_set_external_input(h,0x800)==ESP_ERR_INVALID_ARG);
 for(unsigned i=0;i<101;++i){start(h);assert(!codec_open&&!h->audio_started&&!h->port_cleanup_pending&&!atomic_load(&s_emulator_busy));assert(aether_nes_stop_wait(h,0)==ESP_OK);aether_nes_release_frame(h);assert(!h->frame565);}
 audio_stop_error=ESP_FAIL;start(h);assert(h->state==AETHER_NES_ERROR&&h->audio_started&&codec_open&&atomic_load(&s_emulator_busy));
 assert(aether_nes_start(h,fixture,frame,NULL)==ESP_ERR_INVALID_STATE);
 unsigned old=stops;assert(aether_nes_stop_wait(h,0)==ESP_ERR_TIMEOUT&&stops==old);assert(aether_nes_stop_wait(h,500)==ESP_ERR_TIMEOUT&&stops==old);
 assert(aether_nes_stop_wait(h,2000)==ESP_FAIL&&h->audio_started&&atomic_load(&s_emulator_busy));
 audio_stop_error=ESP_OK;assert(aether_nes_stop_wait(h,2000)==ESP_OK&&!h->audio_started&&!codec_open&&!atomic_load(&s_emulator_busy));
 port_stop_error=ESP_FAIL;start(h);assert(h->state==AETHER_NES_ERROR&&h->port_cleanup_pending&&atomic_load(&s_emulator_busy));
 assert(aether_nes_stop_wait(h,2000)==ESP_FAIL&&h->port_cleanup_pending);assert(aether_nes_start(h,fixture,frame,NULL)==ESP_ERR_INVALID_STATE);
 port_stop_error=ESP_OK;assert(aether_nes_stop_wait(h,2000)==ESP_OK&&!h->port_cleanup_pending&&!atomic_load(&s_emulator_busy));
 h->stop_barrier=true;assert(aether_nes_start(h,fixture,frame,NULL)==ESP_ERR_INVALID_STATE);assert(aether_nes_stop_wait(h,0)==ESP_ERR_INVALID_STATE);h->stop_barrier=false;
 start(h);assert(aether_nes_stop_wait(h,0)==ESP_OK);assert(aether_nes_del(h)==ESP_OK&&!codec_open&&!leases);
 h=new_player();save_failure=true;start(h);assert(h->state==AETHER_NES_ERROR&&h->last_error==ESP_FAIL&&!leases&&!codec_open&&!atomic_load(&s_emulator_busy));
 assert(aether_nes_stop_wait(h,0)==ESP_OK);save_failure=false;start(h);assert(h->state==AETHER_NES_STOPPED&&h->persistence_error==ESP_OK&&h->last_error==ESP_OK);assert(aether_nes_del(h)==ESP_OK);
 assert(starts==106);remove(fixture);puts("PASS real NES worker101 relaunches, priority7, external API/session reset, audio/timer teardown faults preserve ownership and stop barrier blocks reentry/media handoff until recovery");puts("PASS persistence failure is reported even on requested stop while files/codec/session ownership release normally and next launch clears the stale failure");return 0;
}
