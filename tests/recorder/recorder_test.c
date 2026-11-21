#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void *test_calloc(size_t,size_t);
static size_t test_fwrite(const void *,size_t,size_t,FILE *);
static int test_fflush(FILE *),test_fsync(int),test_fclose(FILE *),test_ftruncate(int,off_t);
#define calloc test_calloc
#define fwrite test_fwrite
#define fflush test_fflush
#define fsync test_fsync
#define fclose test_fclose
#define ftruncate test_ftruncate
#include "../../components/aether_recorder/aether_recorder.c"
#undef calloc
#undef fwrite
#undef fflush
#undef fsync
#undef fclose
#undef ftruncate

struct test_task { pthread_t thread;void(*run)(void*);void *argument; };
static struct test_task *tasks[128];static unsigned task_count;
static atomic_bool inserted=true,codec_on,hold_read,read_entered,hold_write,write_entered;
static atomic_bool fail_start,fail_calloc,fail_writer,fail_collector,fail_flush,fail_sync,fail_close,fail_truncate;
static atomic_uint reads,starts,stops,leases,sync_calls,allocation_index,fail_allocation,fail_read_at,fail_stops;
static atomic_bool partial_write;
static atomic_llong clock_offset;

void vTaskDelay(TickType_t ms){struct timespec t={ms/1000,(long)(ms%1000)*1000000};nanosleep(&t,NULL);}
TickType_t xTaskGetTickCount(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (TickType_t)(t.tv_sec*1000ULL+t.tv_nsec/1000000);}
int64_t esp_timer_get_time(void){return (int64_t)xTaskGetTickCount()*1000+atomic_load(&clock_offset);}
void test_yield(void){sched_yield();}
void vTaskDelete(TaskHandle_t unused){(void)unused;pthread_exit(NULL);}
static void *run_task(void *argument){struct test_task *t=argument;t->run(t->argument);return NULL;}
BaseType_t xTaskCreate(void(*fn)(void*),const char *name,uint32_t stack,void *ctx,int priority,TaskHandle_t *out)
{
    assert(stack==4096);
    bool collector=!strcmp(name,"rec_capture");assert(priority==(collector?5:3));
    if(atomic_exchange(collector?&fail_collector:&fail_writer,false))return pdFALSE;
    struct test_task *t=calloc(1,sizeof(*t));assert(t&&task_count<128);t->run=fn;t->argument=ctx;tasks[task_count++]=t;
    if(out)*out=t;
    assert(!pthread_create(&t->thread,NULL,run_task,t));return pdPASS;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void){pthread_mutex_t *m=malloc(sizeof(*m));assert(m&&!pthread_mutex_init(m,NULL));return m;}
BaseType_t xSemaphoreTake(SemaphoreHandle_t m,TickType_t ms)
{
    if(ms==portMAX_DELAY)return !pthread_mutex_lock(m);
    if(!ms)return !pthread_mutex_trylock(m);
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=ms/1000;until.tv_nsec+=(long)(ms%1000)*1000000;
    until.tv_sec+=until.tv_nsec/1000000000;until.tv_nsec%=1000000000;
    int e=pthread_mutex_timedlock(m,&until);assert(!e||e==ETIMEDOUT);return !e;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m){assert(!pthread_mutex_unlock(m));return pdTRUE;}
void vSemaphoreDelete(SemaphoreHandle_t m){assert(!pthread_mutex_destroy(m));free(m);}
static void *test_calloc(size_t n,size_t size){if(atomic_exchange(&fail_calloc,false))return NULL;return calloc(n,size);}
void *heap_caps_malloc(size_t bytes,int caps){assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));unsigned n=++allocation_index;return n==fail_allocation?NULL:malloc(bytes);}
esp_err_t aether_storage_acquire(aether_storage_handle_t s,uint32_t timeout){assert(s==(void*)2&&timeout==0);if(!inserted)return ESP_ERR_INVALID_STATE;++leases;return ESP_OK;}
void aether_storage_release(aether_storage_handle_t s){assert(s==(void*)2&&leases==1);--leases;}
bool aether_storage_card_inserted(aether_storage_handle_t s){assert(s==(void*)2);return inserted;}
esp_err_t ac101s_audio_capture_start(ac101s_audio_handle_t a,float gain)
{assert(a==(void*)1&&gain==24&&!atomic_exchange(&codec_on,true));++starts;return atomic_exchange(&fail_start,false)?ESP_FAIL:ESP_OK;}
esp_err_t ac101s_audio_capture_read(ac101s_audio_handle_t a,void *out,size_t bytes)
{
    assert(a==(void*)1&&codec_on&&bytes==4096);unsigned n=++reads;
    if(hold_read){read_entered=true;while(hold_read)vTaskDelay(1);}
    vTaskDelay(2);
    int16_t *pcm=out;const int16_t pattern[]={-32768,1234,0,32767};
    for(unsigned i=0;i<bytes/4;++i){pcm[i*2]=pattern[i%4];const int16_t right[]={-32768,-2222,32767,32767};pcm[i*2+1]=right[i%4];}
    return fail_read_at&&n>=fail_read_at?ESP_FAIL:ESP_OK;
}
esp_err_t ac101s_audio_capture_stop(ac101s_audio_handle_t a)
{assert(a==(void*)1);++stops;if(fail_stops){--fail_stops;return ESP_FAIL;}codec_on=false;return ESP_OK;}
static size_t test_fwrite(const void *p,size_t size,size_t count,FILE *f)
{
    assert(size==1);
    if(count!=44) {
        if(hold_write){write_entered=true;while(hold_write)vTaskDelay(1);}
        if(atomic_exchange(&partial_write,false)){errno=ENOSPC;return fwrite(p,1,1235,f);}
    }
    return fwrite(p,size,count,f);
}
static int test_fflush(FILE *f){if(atomic_exchange(&fail_flush,false)){errno=EIO;return EOF;}return fflush(f);}
static int test_fsync(int fd){++sync_calls;if(atomic_exchange(&fail_sync,false)){errno=EIO;return -1;}return fsync(fd);}
static int test_fclose(FILE *f){int result=fclose(f);if(atomic_exchange(&fail_close,false)){errno=EIO;return EOF;}return result;}
static int test_ftruncate(int fd,off_t length){if(atomic_exchange(&fail_truncate,false)){errno=EIO;return -1;}return ftruncate(fd,length);}

static aether_recorder_status_t snapshot(aether_recorder_handle_t h){aether_recorder_status_t s;assert(aether_recorder_get_status(h,&s)==ESP_OK);return s;}
static aether_recorder_status_t wait_state(aether_recorder_handle_t h,aether_recorder_state_t state)
{for(unsigned i=0;i<5000;++i){aether_recorder_status_t s=snapshot(h);if(s.state==state)return s;vTaskDelay(1);}assert(!"state timeout");return snapshot(h);}
static void wait_flag(atomic_bool *flag){for(unsigned i=0;i<5000&&!atomic_load(flag);++i)vTaskDelay(1);assert(atomic_load(flag));}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void verify_wav(aether_recorder_status_t s)
{
    FILE *f=fopen(s.path,"rb");assert(f);uint8_t header[44];assert(fread(header,1,44,f)==44);
    assert(!memcmp(header,"RIFF",4)&&!memcmp(header+8,"WAVEfmt ",8)&&!memcmp(header+36,"data",4));
    assert(get32(header+4)==s.data_bytes+36&&get32(header+24)==48000&&get32(header+28)==96000&&get32(header+40)==s.data_bytes);
    assert(header[20]==1&&header[22]==1&&header[32]==2&&header[34]==16&&!(s.data_bytes&1));
    assert(!fseek(f,0,SEEK_END)&&(uint64_t)ftell(f)==44+s.data_bytes);rewind(f);fseek(f,44,SEEK_SET);
    const int16_t pattern[]={-32768,-494,16383,32767};
    for(uint64_t i=0;i<s.data_bytes/2;++i){int lo=fgetc(f),hi=fgetc(f);assert(lo>=0&&hi>=0);assert((int16_t)(lo|(hi<<8))==pattern[i%4]);}
    fclose(f);assert(s.duration_ms==s.data_bytes*1000/96000);
}
static void wait_data(aether_recorder_handle_t h)
{for(unsigned i=0;i<2000&&snapshot(h).data_bytes<8192;++i)vTaskDelay(1);assert(snapshot(h).data_bytes>=8192);}
static void assert_released(aether_recorder_handle_t h)
{assert(!snapshot(h).active&&!codec_on&&!leases);assert(aether_recorder_stop_wait(h,0)==ESP_OK);}
struct stop_job{aether_recorder_handle_t h;esp_err_t result;};
static void *barrier(void *arg){struct stop_job *j=arg;j->result=aether_recorder_stop_wait(j->h,3000);return NULL;}

static void test_recording(aether_recorder_handle_t h)
{
    assert(aether_recorder_start(h)==ESP_OK);wait_state(h,AETHER_RECORDER_RECORDING);wait_data(h);assert(snapshot(h).peak==32768);
    assert(aether_recorder_set_paused(h,true)==ESP_OK);wait_state(h,AETHER_RECORDER_PAUSED);vTaskDelay(20);
    aether_recorder_status_t before=snapshot(h);unsigned previous_reads=reads;vTaskDelay(30);
    assert(snapshot(h).data_bytes==before.data_bytes&&reads>previous_reads&&snapshot(h).peak==0&&snapshot(h).active);
    assert(aether_recorder_set_paused(h,false)==ESP_OK);wait_state(h,AETHER_RECORDER_RECORDING);vTaskDelay(10);
    unsigned synced=sync_calls;clock_offset+=2100000;
    for(unsigned i=0;i<2000&&sync_calls==synced;++i)vTaskDelay(1);
    assert(sync_calls>synced);
    FILE *checkpoint=fopen(snapshot(h).path,"rb");assert(checkpoint);uint8_t head[44];assert(fread(head,1,44,checkpoint)==44);assert(get32(head+40)>0);fclose(checkpoint);
    read_entered=false;hold_read=true;wait_flag(&read_entered);
    assert(aether_recorder_stop_wait(h,0)==ESP_ERR_TIMEOUT&&snapshot(h).active&&leases==1);
    assert(aether_recorder_stop_wait(h,10)==ESP_ERR_TIMEOUT&&snapshot(h).active);
    struct stop_job job={.h=h};pthread_t t;assert(!pthread_create(&t,NULL,barrier,&job));vTaskDelay(5);
    assert(aether_recorder_start(h)==ESP_ERR_INVALID_STATE);hold_read=false;pthread_join(t,NULL);assert(job.result==ESP_OK);
    aether_recorder_status_t saved=wait_state(h,AETHER_RECORDER_FINISHED);assert_released(h);verify_wav(saved);
    assert(aether_recorder_start(h)==ESP_OK);wait_state(h,AETHER_RECORDER_RECORDING);wait_data(h);assert(strcmp(saved.path,snapshot(h).path));
    assert(aether_recorder_stop_wait(h,3000)==ESP_OK);verify_wav(snapshot(h));verify_wav(saved);
    puts("PASS real writer/collector: stereo-to-mono WAV including right-only signal and no full-scale overflow, full-scale meter, pause drains RX, resume, checkpoint, unique files and stop ownership barrier");
}
static void test_failures(aether_recorder_handle_t h)
{
    fail_start=true;unsigned previous_stops=stops;assert(aether_recorder_start(h)==ESP_OK);wait_state(h,AETHER_RECORDER_ERROR);
    assert(stops>previous_stops);assert_released(h);verify_wav(snapshot(h));
    for(unsigned n=1;n<=4;++n){allocation_index=0;fail_allocation=n;assert(aether_recorder_start(h)==ESP_OK);wait_state(h,AETHER_RECORDER_ERROR);assert_released(h);assert(snapshot(h).last_error==ESP_ERR_NO_MEM);}fail_allocation=0;
    fail_calloc=true;assert(aether_recorder_start(h)==ESP_OK);wait_state(h,AETHER_RECORDER_ERROR);assert_released(h);
    fail_writer=true;assert(aether_recorder_start(h)==ESP_ERR_NO_MEM);assert_released(h);
    fail_collector=true;assert(aether_recorder_start(h)==ESP_OK);wait_state(h,AETHER_RECORDER_ERROR);assert_released(h);verify_wav(snapshot(h));
    assert(aether_recorder_start(h)==ESP_OK);wait_state(h,AETHER_RECORDER_RECORDING);wait_data(h);fail_read_at=reads+2;
    wait_state(h,AETHER_RECORDER_ERROR);assert_released(h);verify_wav(snapshot(h));fail_read_at=0;
    assert(aether_recorder_start(h)==ESP_OK);wait_state(h,AETHER_RECORDER_RECORDING);wait_data(h);assert(aether_recorder_set_paused(h,true)==ESP_OK);
    wait_state(h,AETHER_RECORDER_PAUSED);inserted=false;wait_state(h,AETHER_RECORDER_ERROR);assert_released(h);verify_wav(snapshot(h));inserted=true;
    assert(aether_recorder_start(h)==ESP_OK);wait_state(h,AETHER_RECORDER_RECORDING);wait_data(h);fail_stops=2;
    assert(aether_recorder_stop_wait(h,10)==ESP_ERR_TIMEOUT&&snapshot(h).active&&leases==1&&codec_on);
    assert(aether_recorder_stop_wait(h,3000)==ESP_OK);assert_released(h);assert(snapshot(h).last_error==ESP_FAIL);verify_wav(snapshot(h));
    puts("PASS startup/PSRAM/task/ADC failures, paused TF removal and failed capture close retain/release exact codec/card ownership");
}
static void test_storage_faults(aether_recorder_handle_t h)
{
    hold_write=true;write_entered=false;assert(aether_recorder_start(h)==ESP_OK);wait_flag(&write_entered);
    for(unsigned i=0;i<3000&&snapshot(h).last_error==ESP_OK;++i)vTaskDelay(1);
    assert(snapshot(h).last_error==ESP_ERR_NO_MEM&&snapshot(h).active&&leases==1);
    assert(aether_recorder_stop_wait(h,10)==ESP_ERR_TIMEOUT);hold_write=false;
    assert(aether_recorder_stop_wait(h,3000)==ESP_OK);assert_released(h);verify_wav(snapshot(h));
    assert(aether_recorder_start(h)==ESP_OK);wait_state(h,AETHER_RECORDER_RECORDING);wait_data(h);partial_write=true;
    wait_state(h,AETHER_RECORDER_ERROR);assert_released(h);verify_wav(snapshot(h));
    atomic_bool *faults[]={&fail_flush,&fail_sync,&fail_close,&fail_truncate};
    for(unsigned i=0;i<4;++i){assert(aether_recorder_start(h)==ESP_OK);wait_state(h,AETHER_RECORDER_RECORDING);wait_data(h);atomic_store(faults[i],true);
        assert(aether_recorder_stop_wait(h,3000)==ESP_OK);assert_released(h);assert(snapshot(h).last_error==ESP_FAIL);if(i!=3)verify_wav(snapshot(h));}
    puts("PASS bounded queue overflow explicitly errors; odd partial ENOSPC truncates to valid PCM; flush/fsync/fclose/ftruncate failures are surfaced");
}
int main(void)
{
    assert(!mkdir(AETHER_STORAGE_MOUNT_POINT,0777)||errno==EEXIST);
    assert(!aether_recorder_get_default());aether_recorder_handle_t h;
    assert(aether_recorder_new((void*)1,(void*)2,&h)==ESP_OK&&aether_recorder_get_default()==h&&task_count==0);
    aether_recorder_handle_t duplicate=(void*)3;assert(aether_recorder_new((void*)1,(void*)2,&duplicate)==ESP_ERR_INVALID_STATE&&!duplicate);
    assert(MAX_DATA_BYTES%2==0&&MAX_DATA_BYTES+44<=INT32_MAX&&MAX_DATA_BYTES+36<=UINT32_MAX);
    assert(aether_recorder_stop_wait(h,0)==ESP_OK);test_recording(h);test_failures(h);test_storage_faults(h);
    assert(aether_recorder_del(h)==ESP_OK&&!aether_recorder_get_default());
    for(unsigned i=0;i<task_count;++i){assert(!pthread_join(tasks[i]->thread,NULL));free(tasks[i]);}
    puts("All production recorder host regressions passed.");return 0;
}
