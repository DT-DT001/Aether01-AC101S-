#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include "../../main/app_nes_controls.c"
static TickType_t now;
static bool stored,busy,task_fail,mutate_in_commit;
static settings_t committed,staged;
static size_t stored_size=sizeof(settings_t);
static unsigned opens,writes,commits,notifies;
static esp_err_t open_error,set_error,commit_error,get_error;
TickType_t xTaskGetTickCount(void){return now;}
void vTaskDelay(TickType_t t){now+=t;}
BaseType_t xTaskCreate(void (*fn)(void*),const char *name,uint32_t stack,void *ctx,int priority,TaskHandle_t *task)
{assert(fn&& !strcmp(name,"nes_keys")&&stack==3072&&!ctx&&priority==2);if(task_fail)return pdFALSE;*task=(void*)1;return pdPASS;}
void xTaskNotifyGive(TaskHandle_t task){assert(task==(void*)1);++notifies;}
uint32_t ulTaskNotifyTake(BaseType_t b,TickType_t t){(void)b;(void)t;return 0;}
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *out){assert(!strcmp(name,"nes_keys"));++opens;if(open_error)return open_error;if(mode==NVS_READONLY&&!stored)return ESP_ERR_NVS_NOT_FOUND;*out=1;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *data,size_t *size){assert(h==1&&!strcmp(key,"mapping"));if(get_error)return get_error;memcpy(data,&committed,*size<sizeof(committed)?*size:sizeof(committed));*size=stored_size;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t size){assert(h==1&&!strcmp(key,"mapping")&&size==sizeof(settings_t));++writes;if(set_error)return set_error;memcpy(&staged,data,size);return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){assert(h==1);++commits;if(mutate_in_commit){mutate_in_commit=false;assert(app_nes_controls_set(APP_NES_CONTROL_A,0,0x0a,NULL)==ESP_OK);}if(commit_error)return commit_error;committed=staged;stored=true;return ESP_OK;}
void nvs_close(nvs_handle_t h){assert(h==1);}
static bool guard(void *context){assert(context==(void*)2);return busy;}
static void init_again(void){s.view.ready=false;s.task=NULL;assert(app_nes_controls_init()==ESP_OK);app_nes_controls_set_persistence_guard(guard,(void*)2);}
static uint16_t map_key(unsigned usage){uint8_t keys[32]={0};keys[usage/8]|=1U<<(usage%8);return app_nes_controls_map(NULL,keys);}
static void *map_thread(void *context){(void)context;uint8_t keys[32]={0};keys[0x0a/8]|=1U<<(0x0a%8);keys[0x1d/8]|=1U<<(0x1d%8);for(unsigned i=0;i<20000;++i)assert(app_nes_controls_map(NULL,keys)==(1U<<APP_NES_CONTROL_A));return NULL;}
int main(void){
 assert(map_key(0x52)==1&&map_key(0x1a)==1&&map_key(0x29)==1024);
 assert(app_nes_controls_init()==ESP_OK);app_nes_controls_set_persistence_guard(guard,(void*)2);
 app_nes_controls_snapshot_t v;app_nes_controls_get_snapshot(&v);assert(v.ready&&!v.pending&&v.error==ESP_OK);
 for(unsigned a=0;a<APP_NES_CONTROL_COUNT;++a)for(unsigned k=0;k<2;++k)if(defaults[a][k])assert(map_key(defaults[a][k])==(1U<<a));
 for(unsigned i=0;i<app_nes_controls_key_count();++i)assert(app_nes_controls_key_index(app_nes_controls_key_usage(i))==i);
 puts("PASS all11 default actions/aliases and key names resolve actual HID usage bits; no-init failure retains usable defaults");
 unsigned conflict=99;const uint32_t rev=v.revision;
 assert(app_nes_controls_set(APP_NES_CONTROL_A,0,0x1b,&conflict)==ESP_ERR_INVALID_ARG&&conflict==APP_NES_CONTROL_B);
 assert(app_nes_controls_set(APP_NES_CONTROL_UP,0,0x1a,&conflict)==ESP_ERR_INVALID_ARG&&conflict==APP_NES_CONTROL_UP);
 assert(app_nes_controls_set(APP_NES_CONTROL_A,2,4,NULL)==ESP_ERR_INVALID_ARG);
 assert(app_nes_controls_set(APP_NES_CONTROL_A,0,0xff,NULL)==ESP_ERR_INVALID_ARG);
 app_nes_controls_get_snapshot(&v);assert(v.revision==rev&&!v.pending);
 assert(app_nes_controls_set(APP_NES_CONTROL_A,0,0x0a,NULL)==ESP_OK&&map_key(0x0a)==16&&map_key(0x1d)==0&&writes==0);
 assert(app_nes_controls_set(APP_NES_CONTROL_B,0,0,NULL)==ESP_OK&&map_key(0x1b)==0);
 assert(app_nes_controls_set(APP_NES_CONTROL_A,1,0x1b,NULL)==ESP_OK&&map_key(0x1b)==16);
 puts("PASS remapping takes effect immediately, primary/secondary/unbound state works and conflicting assignments are rejected atomically");
 busy=true;now=500;assert(service_once()==250&&writes==0);app_nes_controls_get_snapshot(&v);assert(v.deferred&&v.pending);
 busy=false;assert(service_once()==portMAX_DELAY&&writes==1&&commits==1&&stored);
 init_again();assert(map_key(0x0a)==16&&map_key(0x1b)==16&&map_key(0x1d)==0);app_nes_controls_get_snapshot(&v);assert(!v.pending);
 puts("PASS NVS whole-map save/reload preserves custom bindings; active-media guard defers Flash IO without blocking live mapping");
 now=1000;assert(app_nes_controls_reset()==ESP_OK);assert(service_once()==250);now+=250;commit_error=ESP_FAIL;
 assert(service_once()==1000);app_nes_controls_get_snapshot(&v);assert(v.pending&&v.error==ESP_FAIL);
 commit_error=0;mutate_in_commit=true;assert(service_once()==250);app_nes_controls_get_snapshot(&v);assert(v.pending&&map_key(0x0a)==16);
 now+=250;assert(service_once()==portMAX_DELAY);init_again();assert(map_key(0x0a)==16);
 puts("PASS failed commits retain dirty settings and retry; edits during an older commit cannot be marked saved prematurely");
 settings_t good=committed;committed.keys[APP_NES_CONTROL_B][0]=committed.keys[APP_NES_CONTROL_A][0];init_again();app_nes_controls_get_snapshot(&v);assert(v.error==ESP_ERR_INVALID_RESPONSE&&map_key(0x1d)==16);
 committed=good;committed.version=99;init_again();app_nes_controls_get_snapshot(&v);assert(v.error==ESP_ERR_INVALID_RESPONSE);
 committed=good;stored_size=sizeof(committed)-1;init_again();app_nes_controls_get_snapshot(&v);assert(v.error==ESP_ERR_INVALID_RESPONSE);
 stored_size=sizeof(committed);committed=good;init_again();
 now=UINT32_MAX-100;assert(app_nes_controls_reset()==ESP_OK);now+=250;assert(service_once()==portMAX_DELAY);
 init_again();assert(map_key(0x1d)==16&&map_key(0x0a)==0&&map_key(0x29)==1024);
 puts("PASS corrupt/version/length/duplicate NVS data falls back safely; restore defaults persists across reboot and tick wrap");
 assert(app_nes_controls_set(APP_NES_CONTROL_B,0,0,NULL)==ESP_OK);assert(app_nes_controls_set(APP_NES_CONTROL_A,1,0,NULL)==ESP_OK);
 pthread_t reader;assert(!pthread_create(&reader,NULL,map_thread,NULL));
 for(unsigned i=0;i<20000;++i)assert(app_nes_controls_set(APP_NES_CONTROL_A,0,(i&1)?0x1d:0x0a,NULL)==ESP_OK);
 assert(!pthread_join(reader,NULL));
 puts("PASS concurrent keyboard reports and settings changes observe coherent complete mappings without GUI/NVS calls from the USB callback");
 s.view.ready=false;task_fail=true;assert(app_nes_controls_init()==ESP_ERR_NO_MEM);assert(map_key(0x1d)==16);
 puts("PASS persistence-task allocation failure keeps default keyboard controls usable and rejects unsavable UI edits");
}
