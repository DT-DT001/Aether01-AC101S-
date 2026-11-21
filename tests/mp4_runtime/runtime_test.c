#include "aether_mp4_player.h"
#include "aether_mp4_audio.h"
#include "esp_h264_dec_sw.h"
#include "esp_aac_dec.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

struct test_task { pthread_t thread; void (*entry)(void *); void *argument; pthread_mutex_t mutex; pthread_cond_t notified; unsigned notifications;
    char name[16]; bool with_caps,suspended,deleted,joined,waiting_notification; };
struct test_h264 { uint8_t image[32*32*3/2]; bool reference; unsigned serial; };
static struct test_task *tasks[256];
static unsigned task_count;
static _Thread_local struct test_task *current_task;
static atomic_bool block_worker_exit;
static atomic_bool block_audio_suspend,block_video_suspend;
static atomic_uint audio_suspend_entered,video_suspend_entered,caps_created,caps_deleted;
static atomic_bool block_decoder_new,decoder_new_inside,block_frame_free,frame_free_inside;
static atomic_bool fail_video_packet;
static _Atomic(void *) frame_pixels;
static atomic_uint frame_frees;
static atomic_bool inserted=true,decoding,block_decode,audio_on;
static atomic_bool decode_block_entered;
static atomic_uint leases,decoders,aac_decoders,callbacks,audio_writes,audio_stops,heap_calls;
static atomic_int fail_heap,fail_decoder,fail_decode,fail_aac,fail_audio;
static atomic_uint conversion_delay_ms,callback_delay_ms,h264_delay_ms;
static atomic_bool block_publish_take,publish_waiting;
static void delay(unsigned ms);
static unsigned mutex_creations;
static SemaphoreHandle_t publish_mutex;
static atomic_uint task_calls;
static atomic_int video_task_core,audio_task_core;
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
enum { EXPECTED_VIDEO_CORE=0 };
#else
enum { EXPECTED_VIDEO_CORE=1 };
#endif
static atomic_int fail_task;
static atomic_bool audio_start_invalid;
static atomic_ullong audio_frames;
static atomic_uint decoder_serial,last_picture_pts;
static atomic_uint aac_opens,aac_resets,aac_closes,aac_decode_calls;
static atomic_bool fail_aac_reset,block_aac_decode,aac_decoding;
static atomic_bool block_callback,callback_inside,consumer_resetting;
static atomic_uint consumer_resets,pending_picture;
static struct { unsigned decoder,type,pts; } decoded_units[512];
static atomic_uint decoded_count;
static uint32_t callback_pts[128];
static atomic_uint big_reads,probe_reads,audio_start_calls,delayed_start_call,audio_start_delay_ms;
static atomic_bool audio_start_delayed;
static atomic_bool fail_idr_cache;
size_t __real_fread(void *,size_t,size_t,FILE *);
size_t __wrap_fread(void *data,size_t size,size_t count,FILE *file)
{
    if(size*count>=60000) ++big_reads;
    if(size==1 && count==5) ++probe_reads;
    return __real_fread(data,size,count,file);
}
size_t __real___fread_chk(void *,size_t,size_t,size_t,FILE *);
size_t __wrap___fread_chk(void *data,size_t capacity,size_t size,size_t count,FILE *file)
{
    if(size*count>=60000) ++big_reads;
    if(size==1 && count==5) ++probe_reads;
    return __real___fread_chk(data,capacity,size,count,file);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{ pthread_mutex_t *m=malloc(sizeof(*m)); assert(m && !pthread_mutex_init(m,NULL)); if((mutex_creations++&1U)==0) publish_mutex=m; return m; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m,TickType_t timeout)
{
    if(m==publish_mutex && timeout && block_publish_take) { publish_waiting=true; while(block_publish_take) delay(1); publish_waiting=false; }
    if(!timeout) return !pthread_mutex_trylock(m);
    struct timespec deadline; clock_gettime(CLOCK_REALTIME,&deadline);
    deadline.tv_sec+=timeout/1000; deadline.tv_nsec+=(long)(timeout%1000)*1000000;
    if(deadline.tv_nsec>=1000000000) { ++deadline.tv_sec; deadline.tv_nsec-=1000000000; }
    return !pthread_mutex_timedlock(m,&deadline);
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { return !pthread_mutex_unlock(m); }
void vSemaphoreDelete(SemaphoreHandle_t m) { assert(!pthread_mutex_destroy(m)); free(m); }
static pthread_mutex_t log_lock=PTHREAD_MUTEX_INITIALIZER;
static char diagnostic_log[65536];
static unsigned log_count;
void mp4_test_log(const char *level,const char *tag,const char *format,...)
{
    assert(!strcmp(tag,"mp4_player"));
    pthread_mutex_lock(&log_lock);
    size_t used=strlen(diagnostic_log); assert(used+8<sizeof diagnostic_log);
    int prefix=snprintf(diagnostic_log+used,sizeof diagnostic_log-used,"%s ",level); assert(prefix>0); used+=(size_t)prefix;
    va_list args; va_start(args,format);
    int n=vsnprintf(diagnostic_log+used,sizeof diagnostic_log-used,format,args); va_end(args);
    assert(n>=0 && (size_t)n+2<sizeof diagnostic_log-used); used+=(size_t)n;
    diagnostic_log[used]='\n'; diagnostic_log[used+1]=0; ++log_count;
    pthread_mutex_unlock(&log_lock);
}
const char *esp_err_to_name(esp_err_t error)
{
    switch(error) {
    case ESP_OK:return "ESP_OK";
    case ESP_FAIL:return "ESP_FAIL";
    case ESP_ERR_NO_MEM:return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_STATE:return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_FOUND:return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_INVALID_SIZE:return "ESP_ERR_INVALID_SIZE";
    default:return "ESP_ERR_INVALID_RESPONSE";
    }
}
static pthread_mutex_t allocation_lock=PTHREAD_MUTEX_INITIALIZER;
static void *heap[1024];
static unsigned live_heap;
static int64_t times[128];
static void delay(unsigned ms)
{ struct timespec t={.tv_sec=ms/1000,.tv_nsec=(long)(ms%1000)*1000000}; nanosleep(&t,NULL); }
int64_t esp_timer_get_time(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
void __real_free(void *);
void __wrap_free(void *ptr)
{
    if(ptr && ptr==atomic_load(&frame_pixels)) {
        frame_free_inside=true; while(block_frame_free) delay(1);
        frame_pixels=NULL; ++frame_frees; frame_free_inside=false;
    }
    pthread_mutex_lock(&allocation_lock);
    for(unsigned i=0;i<1024;++i) if(ptr && heap[i]==ptr) { heap[i]=NULL; assert(live_heap); --live_heap; break; }
    pthread_mutex_unlock(&allocation_lock); __real_free(ptr);
}
void *heap_caps_malloc(size_t size,unsigned caps)
{
    (void)caps; unsigned call=++heap_calls; if((int)call==fail_heap) return NULL;
    if(size==512U*1024U && fail_video_packet) return NULL;
    if(size==3 && fail_idr_cache) return NULL; /* Twelve video samples, two bits each. */
    void *ptr=malloc(size); if(!ptr) return NULL;
    if(size==480U*480U*2U) frame_pixels=ptr;
    pthread_mutex_lock(&allocation_lock); unsigned i=0; while(i<1024 && heap[i]) ++i;
    assert(i<1024); heap[i]=ptr; ++live_heap; pthread_mutex_unlock(&allocation_lock); return ptr;
}
void *heap_caps_calloc(size_t count,size_t size,unsigned caps)
{ if(count && size>SIZE_MAX/count) return NULL; void *p=heap_caps_malloc(count*size,caps); if(p) memset(p,0,count*size); return p; }
void *heap_caps_realloc(void *ptr,size_t size,unsigned caps)
{
    if(!ptr) return heap_caps_malloc(size,caps);
    unsigned call=++heap_calls; if((int)call==fail_heap) return NULL;
    pthread_mutex_lock(&allocation_lock); unsigned i=0; while(i<1024 && heap[i]!=ptr) ++i; assert(i<1024);
    void *next=realloc(ptr,size); if(next) heap[i]=next; pthread_mutex_unlock(&allocation_lock); return next;
}
static void *run_task(void *argument)
{ struct test_task *task=argument; current_task=task; task->entry(task->argument); return NULL; }
static BaseType_t create_task(void (*entry)(void *),const char *name,uint32_t stack,void *arg,int priority,TaskHandle_t *out,bool with_caps)
{
    assert(stack==24576);
    assert((!strcmp(name,"mp4_aac") && priority==7) || (!strcmp(name,"mp4_video") && priority==5));
    if((int)++task_calls==fail_task) return pdFALSE;
    struct test_task *task=calloc(1,sizeof(*task)); assert(task && task_count<256);
    assert(!pthread_mutex_init(&task->mutex,NULL) && !pthread_cond_init(&task->notified,NULL));
    task->entry=entry; task->argument=arg; task->with_caps=with_caps;
    snprintf(task->name,sizeof task->name,"%s",name);
    tasks[task_count++]=task; if(out) *out=task;
    if(with_caps) ++caps_created;
    assert(!pthread_create(&task->thread,NULL,run_task,task)); return pdPASS;
}
BaseType_t xTaskCreate(void (*entry)(void *),const char *name,uint32_t stack,void *arg,int priority,TaskHandle_t *out)
{ return create_task(entry,name,stack,arg,priority,out,false); }
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *),const char *name,uint32_t stack,void *arg,int priority,TaskHandle_t *out,BaseType_t core)
{
    if(!strcmp(name,"mp4_video")) { assert(core==EXPECTED_VIDEO_CORE); video_task_core=core; }
    else { assert(!strcmp(name,"mp4_aac") && core==0); audio_task_core=core; }
    return xTaskCreate(entry,name,stack,arg,priority,out);
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*entry)(void *),const char *name,uint32_t stack,void *arg,int priority,TaskHandle_t *out,BaseType_t core,unsigned caps)
{
    assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT) && out && !*out);
    if(!strcmp(name,"mp4_video")) { assert(core==EXPECTED_VIDEO_CORE); video_task_core=core; }
    else { assert(!strcmp(name,"mp4_aac") && core==0); audio_task_core=core; }
    return create_task(entry,name,stack,arg,priority,out,true);
}
void vTaskSuspend(TaskHandle_t handle)
{
    assert(!handle && current_task && current_task->with_caps);
    if(!strcmp(current_task->name,"mp4_aac")) { ++audio_suspend_entered; while(block_audio_suspend) delay(1); }
    else { ++video_suspend_entered; while(block_video_suspend) delay(1); }
    pthread_mutex_lock(&current_task->mutex); current_task->suspended=true;
    pthread_cond_broadcast(&current_task->notified);
    while(!current_task->deleted) pthread_cond_wait(&current_task->notified,&current_task->mutex);
    pthread_mutex_unlock(&current_task->mutex); pthread_exit(NULL);
}
eTaskState eTaskGetState(TaskHandle_t handle)
{
    assert(handle); pthread_mutex_lock(&handle->mutex);
    const eTaskState state=handle->deleted ? eDeleted : handle->suspended ? eSuspended : handle->waiting_notification ? eBlocked : eRunning;
    pthread_mutex_unlock(&handle->mutex); return state;
}
void vTaskDeleteWithCaps(TaskHandle_t handle)
{
    /* A self-delete would create an unbounded helper in the real SDK. */
    assert(handle && handle!=current_task && handle->with_caps);
    pthread_mutex_lock(&handle->mutex); assert(handle->suspended && !handle->deleted);
    handle->deleted=true; pthread_cond_broadcast(&handle->notified); pthread_mutex_unlock(&handle->mutex);
    assert(!pthread_join(handle->thread,NULL)); handle->joined=true; ++caps_deleted;
}
void xTaskNotifyGive(TaskHandle_t task)
{
    assert(task); pthread_mutex_lock(&task->mutex); ++task->notifications;
    pthread_cond_signal(&task->notified); pthread_mutex_unlock(&task->mutex);
}
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t timeout)
{
    assert(current_task && timeout==portMAX_DELAY);
    pthread_mutex_lock(&current_task->mutex);
    current_task->waiting_notification=true;
    while(!current_task->notifications) pthread_cond_wait(&current_task->notified,&current_task->mutex);
    current_task->waiting_notification=false;
    const uint32_t count=current_task->notifications;
    if(clear) current_task->notifications=0; else --current_task->notifications;
    pthread_mutex_unlock(&current_task->mutex);
    while(block_worker_exit) delay(1);
    return count;
}
void vTaskDelete(TaskHandle_t handle) { (void)handle; assert(!"MP4 WithCaps workers must not use ordinary or self deletion"); }
void vTaskDelay(TickType_t ms) { delay(ms); }
TickType_t xTaskGetTickCount(void) { return (uint32_t)(esp_timer_get_time()/1000); }
esp_err_t aether_storage_acquire(aether_storage_handle_t h,uint32_t timeout)
{ (void)h; (void)timeout; if(!inserted) return ESP_ERR_INVALID_STATE; ++leases; return ESP_OK; }
void aether_storage_release(aether_storage_handle_t h) { (void)h; assert(leases); --leases; }
bool aether_storage_card_inserted(aether_storage_handle_t h) { (void)h; return inserted; }
esp_err_t ac101s_audio_start(ac101s_audio_handle_t h,int volume,float gain)
{
    assert(h && volume==100 && gain==0);
    const unsigned call=++audio_start_calls;
    if(call==delayed_start_call) { audio_start_delayed=true; delay(audio_start_delay_ms); audio_start_delayed=false; }
    if(audio_start_invalid) return ESP_ERR_INVALID_STATE;
    if(fail_audio) return ESP_FAIL;
    assert(!audio_on); audio_on=true; return ESP_OK;
}
static atomic_bool fail_audio_stop;
esp_err_t ac101s_audio_stop(ac101s_audio_handle_t h) { assert(h); ++audio_stops;if(fail_audio_stop)return ESP_FAIL;audio_on=false;return ESP_OK; }
esp_err_t ac101s_audio_write(ac101s_audio_handle_t h,void *pcm,size_t bytes)
{
    assert(h && pcm && bytes && bytes<=4096 && !(bytes%4) && audio_on);
    if(fail_audio) return ESP_FAIL;
    for(size_t i=0;i<bytes/2;++i) assert(((int16_t *)pcm)[i]==0);
    ++audio_writes; audio_frames+=bytes/4; delay((unsigned)(bytes*1000/(48000*4))); return ESP_OK;
}
esp_h264_err_t esp_h264_dec_sw_new(const esp_h264_dec_cfg_sw_t *cfg,esp_h264_dec_handle_t *out)
{
    assert(cfg->pic_type==ESP_H264_RAW_FMT_I420); decoder_new_inside=true;
    while(block_decoder_new) delay(1);
    decoder_new_inside=false;
    if(fail_decoder) return ESP_H264_ERR_MEM;
    *out=calloc(1,sizeof(**out)); assert(*out); (*out)->serial=++decoder_serial; ++decoders;
    return ESP_H264_ERR_OK;
}
esp_h264_err_t esp_h264_dec_open(esp_h264_dec_handle_t decoder) { assert(decoder); return ESP_H264_ERR_OK; }
esp_h264_err_t esp_h264_dec_close(esp_h264_dec_handle_t decoder) { assert(decoder); return ESP_H264_ERR_OK; }
esp_h264_err_t esp_h264_dec_del(esp_h264_dec_handle_t decoder) { assert(decoder && decoders); free(decoder); --decoders; return ESP_H264_ERR_OK; }
esp_h264_err_t esp_h264_dec_sw_get_param_hd(esp_h264_dec_handle_t decoder,esp_h264_dec_param_sw_handle_t *out)
{ *out=decoder; return ESP_H264_ERR_OK; }
esp_h264_err_t esp_h264_dec_get_resolution(esp_h264_dec_param_sw_handle_t decoder,esp_h264_resolution_t *out)
{ assert(decoder); if(conversion_delay_ms) delay(conversion_delay_ms); *out=(esp_h264_resolution_t){32,32}; return ESP_H264_ERR_OK; }
esp_h264_err_t esp_h264_dec_process(esp_h264_dec_handle_t decoder,esp_h264_dec_in_frame_t *in,esp_h264_dec_out_frame_t *out)
{
    assert(decoder && in->raw_data.len>=5 && in->raw_data.buffer[3]==1);
    decoding=true;
    while(block_decode) { decode_block_entered=true; delay(1); }
    decode_block_entered=false;
    if(fail_decode) { decoding=false; return ESP_H264_ERR_FAIL; }
    unsigned size=in->raw_data.len;
    for(unsigned i=4;i+4<size;++i) if(!in->raw_data.buffer[i] && !in->raw_data.buffer[i+1] &&
        !in->raw_data.buffer[i+2] && in->raw_data.buffer[i+3]==1) { size=i; break; }
    in->consume=size;
    /* Real tinyh264 compacts escaped RBSP into its writable input. Model the
     * destructive input contract, which the original no-mutation mock missed.
     * A reused SPS cache now fails instead of making seek appear successful. */
    if((in->raw_data.buffer[4]&31)==7) {
        if(in->raw_data.buffer[5]!=66) { decoding=false; return ESP_H264_ERR_FAIL; }
        in->raw_data.buffer[5]=0xff;
    }
    if((in->raw_data.buffer[4]&31)==5 || (in->raw_data.buffer[4]&31)==1) {
        const unsigned type=in->raw_data.buffer[4]&31;
        if(type==5) decoder->reference=true; else assert(decoder->reference);
        const unsigned event=decoded_count++; assert(event<512);
        decoded_units[event].decoder=decoder->serial; decoded_units[event].type=type; decoded_units[event].pts=in->pts;
        last_picture_pts=in->pts;
        memset(decoder->image,128,sizeof(decoder->image)); memset(decoder->image,16,32*32);
        out->outbuf=decoder->image; out->out_size=sizeof(decoder->image); delay(h264_delay_ms);
    }
    decoding=false; return ESP_H264_ERR_OK;
}
typedef struct { esp_aac_dec_cfg_t cfg; unsigned units; } aac_t;
esp_audio_err_t esp_aac_dec_open(void *configuration,uint32_t bytes,void **out)
{
    assert(bytes==sizeof(esp_aac_dec_cfg_t)); if(fail_aac) return ESP_AUDIO_ERR_FAIL;
    aac_t *a=calloc(1,sizeof(*a)); assert(a); a->cfg=*(esp_aac_dec_cfg_t *)configuration; *out=a; ++aac_decoders; ++aac_opens; return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_aac_dec_decode(void *decoder,esp_audio_dec_in_raw_t *in,esp_audio_dec_out_frame_t *out,esp_audio_dec_info_t *info)
{
    aac_t *a=decoder; assert(a && in->len==6 && !memcmp(in->buffer,"\x21\0\0\0\0\x1c",6));
    aac_decoding=true; while(block_aac_decode) delay(1); aac_decoding=false;
    if(fail_aac) return ESP_AUDIO_ERR_FAIL;
    ++a->units; ++aac_decode_calls;
    assert(out->len>=1024U*a->cfg.channel*2); memset(out->buffer,0,1024*a->cfg.channel*2);
    out->decoded_size=1024*a->cfg.channel*2; in->consumed=in->len;
    *info=(esp_audio_dec_info_t){.sample_rate=(uint32_t)a->cfg.sample_rate,.channel=a->cfg.channel,.bits_per_sample=16,.frame_size=1024};
    return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_aac_dec_reset(void *decoder)
{ assert(decoder && !aac_decoding); ++aac_resets; if(fail_aac_reset) return ESP_AUDIO_ERR_FAIL; ((aac_t *)decoder)->units=0; return ESP_AUDIO_ERR_OK; }
esp_audio_err_t esp_aac_dec_close(void *decoder)
{ assert(decoder && aac_decoders && !aac_decoding); free(decoder); --aac_decoders; ++aac_closes; return ESP_AUDIO_ERR_OK; }
static void frame(const uint16_t *pixels,uint16_t width,uint16_t height,uint32_t number,void *context)
{ (void)context; assert(width==32 && height==32 && pixels[0]==0 && number);
  assert(!consumer_resetting); callback_inside=true; while(block_callback) delay(1);
  if(callback_delay_ms) delay(callback_delay_ms);
  unsigned i=callbacks; if(i<128) { times[i]=esp_timer_get_time(); callback_pts[i]=last_picture_pts; }
  pending_picture=last_picture_pts+1; ++callbacks; callback_inside=false; }
static aether_mp4_status_t wait_state(aether_mp4_player_handle_t h,aether_mjpeg_state_t state)
{
    aether_mp4_status_t status; int64_t deadline=esp_timer_get_time()+3000000;
    do { assert(aether_mp4_player_get_status(h,&status)==ESP_OK); if(status.state==state) return status; delay(2); } while(esp_timer_get_time()<deadline);
    fprintf(stderr,"expected %d got %d err=%x detail=%s callbacks=%u\n",state,status.state,status.last_error,status.detail,callbacks); abort();
}
static void wait_frames(unsigned count)
{ int64_t end=esp_timer_get_time()+2000000; while(callbacks<count && esp_timer_get_time()<end) delay(1); assert(callbacks>=count); }
static aether_mp4_player_handle_t start(const char *path)
{
    aether_mp4_player_handle_t h=NULL; assert(aether_mp4_player_new((void *)1,(void *)1,&h)==ESP_OK);
    assert(aether_mp4_player_play(h,path,frame,NULL)==ESP_OK); return h;
}
static void destroy(aether_mp4_player_handle_t h)
{
    assert(aether_mp4_player_del(h)==ESP_OK);
    for(unsigned i=0;i<task_count;++i) {
        assert(tasks[i]->with_caps && tasks[i]->deleted && tasks[i]->joined);
        assert(!pthread_cond_destroy(&tasks[i]->notified) && !pthread_mutex_destroy(&tasks[i]->mutex)); free(tasks[i]);
    } task_count=0;
    assert(!leases && !decoders && !aac_decoders && !audio_on && !live_heap);
    assert(caps_created==caps_deleted);
}
static void reset(void)
{
    assert(!task_count && !live_heap); inserted=true; decoding=block_decode=audio_on=decode_block_entered=false;
    callbacks=audio_writes=audio_stops=heap_calls=0; audio_frames=decoder_serial=decoded_count=0; block_callback=callback_inside=consumer_resetting=false; consumer_resets=pending_picture=0; fail_heap=fail_decoder=fail_decode=fail_aac=fail_audio=0;
    fail_task=task_calls=0; audio_start_invalid=false; diagnostic_log[0]=0; log_count=0;
    video_task_core=audio_task_core=-2;
    big_reads=probe_reads=audio_start_calls=delayed_start_call=audio_start_delay_ms=0; audio_start_delayed=false; fail_audio_stop=false; fail_idr_cache=false;
    aac_opens=aac_resets=aac_closes=aac_decode_calls=0; fail_aac_reset=block_aac_decode=aac_decoding=false;
    block_worker_exit=false;
    block_audio_suspend=block_video_suspend=false;
    audio_suspend_entered=video_suspend_entered=caps_created=caps_deleted=0;
    block_decoder_new=decoder_new_inside=block_frame_free=frame_free_inside=false;
    fail_video_packet=false;
    frame_pixels=NULL; frame_frees=mutex_creations=0; publish_mutex=NULL;
    conversion_delay_ms=callback_delay_ms=0;
    h264_delay_ms=2; block_publish_take=publish_waiting=false;
}
static void wait_seek(aether_mp4_player_handle_t h)
{
    const int64_t deadline=esp_timer_get_time()+3000000;
    aether_mp4_status_t status;
    do {
        assert(aether_mp4_player_get_status(h,&status)==ESP_OK);
        if(!status.seeking) { assert(status.state!=AETHER_MJPEG_ERROR); return; }
        delay(1);
    } while(esp_timer_get_time()<deadline);
    fprintf(stderr,"seek stalled: %d err=%x detail=%s\n",status.state,status.last_error,status.detail); abort();
}
static esp_err_t seek_retry(aether_mp4_player_handle_t h,uint32_t ms)
{
    for(unsigned i=0;i<1000;++i) {
        esp_err_t error=aether_mp4_player_seek(h,ms);
        if(error!=ESP_ERR_INVALID_STATE) return error;
        delay(1);
    }
    return ESP_ERR_TIMEOUT;
}
static void clear_consumer(void *context)
{
    assert(context==&consumer_resets && !callback_inside);
    consumer_resetting=true;
    /* Exaggerate the caller reset interval: a fast worker must not publish a
     * target preview before this last copied-image invalidation has finished. */
    delay(40); pending_picture=0; ++consumer_resets; consumer_resetting=false;
}
static esp_err_t seek_reset_retry(aether_mp4_player_handle_t h,uint32_t ms)
{
    for(unsigned i=0;i<1000;++i) {
        esp_err_t error=aether_mp4_player_seek_with_reset(h,ms,clear_consumer,&consumer_resets);
        if(error!=ESP_ERR_INVALID_STATE) return error;
        delay(1);
    }
    return ESP_ERR_TIMEOUT;
}
static void wait_audio_start_delay(void)
{
    const int64_t until=esp_timer_get_time()+2000000;
    while(!audio_start_delayed && esp_timer_get_time()<until) delay(1);
    assert(audio_start_delayed);
}
static void slow_next_audio_start(void)
{ delayed_start_call=audio_start_calls+1; audio_start_delay_ms=650; }
static void wait_picture_pts(unsigned pts)
{
    const int64_t until=esp_timer_get_time()+2000000;
    while((!callbacks || callback_pts[callbacks-1]!=pts) && esp_timer_get_time()<until) delay(1);
    assert(callbacks && callback_pts[callbacks-1]==pts);
}
static void wait_converted(aether_mp4_player_handle_t h,unsigned count)
{
    const int64_t until=esp_timer_get_time()+2000000;
    aether_mp4_status_t status;
    do {
        assert(aether_mp4_player_get_status(h,&status)==ESP_OK);
        if(status.converted_frames>=count) return;
        delay(1);
    } while(esp_timer_get_time()<until);
    assert(!"prepared RGB did not become ready");
}
static void assert_reference_sequence(unsigned count)
{
    assert(decoded_count==count && decoder_serial==1);
    for(unsigned i=0;i<count;++i) {
        assert(decoded_units[i].decoder==1 && decoded_units[i].pts==i*250/3);
        assert(decoded_units[i].type==(i%6 ? 1U : 5U));
    }
}
static void wait_flag(atomic_bool *flag)
{
    const int64_t until=esp_timer_get_time()+2000000;
    while(!atomic_load(flag) && esp_timer_get_time()<until) delay(1);
    assert(atomic_load(flag));
}
static void wait_counter(atomic_uint *counter,unsigned count)
{
    const int64_t until=esp_timer_get_time()+2000000;
    while(atomic_load(counter)<count && esp_timer_get_time()<until) delay(1);
    assert(atomic_load(counter)>=count);
}
static esp_err_t play_retry(aether_mp4_player_handle_t h,const char *path)
{
    const int64_t until=esp_timer_get_time()+2000000;
    esp_err_t error;
    do { error=aether_mp4_player_play(h,path,frame,NULL); if(error!=ESP_ERR_INVALID_STATE) return error; delay(1); }
    while(esp_timer_get_time()<until);
    return error;
}
typedef struct { aether_mp4_player_handle_t h; uint32_t timeout; esp_err_t result; atomic_bool entered; } stop_request_t;
static void *request_stop(void *argument)
{
    stop_request_t *r=argument; atomic_store(&r->entered,true);
    r->result=aether_mp4_player_stop_wait(r->h,r->timeout); return NULL;
}
static void test_hp_lifecycle(const char *root)
{
    char video[512],av[512]; snprintf(video,sizeof video,"%s/baseline-gop.mp4",root);
    snprintf(av,sizeof av,"%s/baseline-gop-aac.mp4",root);
    reset(); block_audio_suspend=true; aether_mp4_player_handle_t h=start(av); wait_frames(2);
    assert(aether_mp4_player_stop_wait(h,40)==ESP_ERR_TIMEOUT); wait_counter(&audio_suspend_entered,1);
    assert(caps_created==2 && !caps_deleted && leases && aac_decoders && decoders);
    assert(eTaskGetState(tasks[1])==eRunning && aether_mp4_player_play(h,av,frame,NULL)==ESP_ERR_INVALID_STATE);
    aether_mp4_player_release_frame(h); block_audio_suspend=false;
    assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK && caps_deleted==2 && !live_heap);
    destroy(h);
    puts("PASS AAC final ACK before kernel suspend retains its HP stack, decoder, session and lease; late suspension permits one external WithCaps delete and final release");

    reset(); block_video_suspend=true; h=start(video); wait_frames(2);
    assert(aether_mp4_player_stop_wait(h,40)==ESP_ERR_TIMEOUT); wait_counter(&video_suspend_entered,1);
    assert(caps_created==1 && !caps_deleted && !leases && !decoders && eTaskGetState(tasks[0])==eRunning);
    const int64_t command=esp_timer_get_time();
    assert(aether_mp4_player_play(h,video,frame,NULL)==ESP_ERR_INVALID_STATE && esp_timer_get_time()-command<10000);
    assert(aether_mp4_player_del(h)==ESP_ERR_TIMEOUT && !caps_deleted);
    block_video_suspend=false; assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK && caps_deleted==1);
    assert(aether_mp4_player_stop_wait(h,0)==ESP_OK && caps_deleted==1);
    assert(play_retry(h,video)==ESP_OK); wait_state(h,AETHER_MJPEG_FINISHED); destroy(h);
    puts("PASS video ACK without kernel suspend rejects nonblocking replay and keeps the handle on delete timeout; retry reaps once before a later replay");

    reset(); block_decoder_new=true; h=start(av); wait_flag(&decoder_new_inside);
    assert(caps_created==2 && !audio_start_calls && !aac_opens && !audio_on);
    const int64_t parked_deadline=esp_timer_get_time()+2000000;
    while(eTaskGetState(tasks[1])!=eBlocked && esp_timer_get_time()<parked_deadline) delay(1);
    assert(eTaskGetState(tasks[1])==eBlocked && !decoders);
    assert(aether_mp4_player_stop_wait(h,40)==ESP_ERR_TIMEOUT);
    aether_mp4_player_release_frame(h); block_decoder_new=false;
    assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK && caps_deleted==2 && !aac_opens && !audio_start_calls);
    destroy(h);
    puts("PASS AAC HP stack reserves and parks at epoch zero before H264 opens; STOP before the first segment joins it without touching codec/I2S");

    reset(); fail_video_packet=true; block_audio_suspend=true; h=start(av); wait_counter(&audio_suspend_entered,1);
    assert(caps_created==2 && !caps_deleted && !aac_opens && !audio_start_calls && leases);
    block_audio_suspend=false; aether_mp4_status_t status=wait_state(h,AETHER_MJPEG_ERROR);
    assert(status.last_error==ESP_ERR_NO_MEM); destroy(h);
    puts("PASS post-reservation video-buffer OOM joins the never-started AAC task through real kernel suspension without phantom hardware or leaked HP stacks");

    reset(); fail_task=2; h=start(av); status=wait_state(h,AETHER_MJPEG_ERROR);
    assert(status.last_error==ESP_ERR_NO_MEM && caps_created==1 && !aac_opens && task_calls==2);
    fail_task=0; assert(play_retry(h,av)==ESP_OK); wait_frames(2);
    assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK && caps_created==3 && caps_deleted==3); destroy(h);
    reset(); fail_task=1; assert(aether_mp4_player_new((void *)1,(void *)1,&h)==ESP_OK);
    assert(aether_mp4_player_play(h,av,frame,NULL)==ESP_ERR_NO_MEM && !caps_created);
    fail_task=0; assert(play_retry(h,av)==ESP_OK); wait_frames(2);
    assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK && caps_created==2 && caps_deleted==2); destroy(h);
    puts("PASS explicit HP video/AAC stack allocation failures leave NULL handles and permit clean retry without falling back to LP memory");

    reset(); block_audio_suspend=true; h=start(av); wait_frames(2);
    stop_request_t first={.h=h,.timeout=2000},second={.h=h,.timeout=30}; pthread_t one,two;
    assert(!pthread_create(&one,NULL,request_stop,&first)); wait_counter(&audio_suspend_entered,1);
    const int64_t before=esp_timer_get_time();
    assert(aether_mp4_player_play(h,av,frame,NULL)==ESP_ERR_INVALID_STATE && esp_timer_get_time()-before<10000);
    assert(!pthread_create(&two,NULL,request_stop,&second)); assert(!pthread_join(two,NULL));
    assert(second.result==ESP_ERR_TIMEOUT && esp_timer_get_time()-before<150000 && !caps_deleted);
    aether_mp4_player_release_frame(h); block_audio_suspend=false;
    assert(!pthread_join(one,NULL) && first.result==ESP_OK && caps_deleted==2);
    assert(play_retry(h,av)==ESP_OK); wait_frames(callbacks+1);
    first=(stop_request_t){.h=h,.timeout=2000}; second=(stop_request_t){.h=h,.timeout=2000};
    assert(!pthread_create(&one,NULL,request_stop,&first) && !pthread_create(&two,NULL,request_stop,&second));
    assert(!pthread_join(one,NULL) && !pthread_join(two,NULL));
    assert(first.result==ESP_OK && second.result==ESP_OK && caps_created==4 && caps_deleted==4); destroy(h);
    puts("PASS lifecycle mutex bounds concurrent-stop waiting, rejects replay during cleanup, and serializes two successful reapers with exactly one delete per HP task");

    reset(); h=start(video); wait_frames(2); block_frame_free=true;
    aether_mp4_player_release_frame(h); assert(aether_mp4_player_stop(h)==ESP_OK); wait_flag(&frame_free_inside);
    aether_mp4_player_release_frame(h); /* Arrives after pixels detach but before its free completes. */
    assert(aether_mp4_player_stop_wait(h,30)==ESP_ERR_TIMEOUT && !caps_deleted && frame_pixels);
    block_frame_free=false; assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK && frame_frees==1 && !live_heap);
    assert(play_retry(h,video)==ESP_OK); wait_state(h,AETHER_MJPEG_FINISHED); destroy(h);
    assert(frame_frees==2);
    puts("PASS release_frame during final detached-buffer free cannot lose its request or publish an early final ACK; retry/replay frees each frame and HP task once");
}
static void test_prepared_video(const char *root)
{
    char path[512]; snprintf(path,sizeof path,"%s/baseline-gop.mp4",root);
    reset(); aether_mp4_player_handle_t h=start(path); wait_converted(h,2);
    assert(callbacks==1 && decoded_count==2);
    assert(aether_mp4_player_set_paused(h,true)==ESP_OK); wait_state(h,AETHER_MJPEG_PAUSED);
    delay(150); assert(callbacks==1 && decoded_count==2);
    assert(aether_mp4_player_set_paused(h,false)==ESP_OK); wait_frames(2);
    assert(callback_pts[1]==83); aether_mp4_status_t status=wait_state(h,AETHER_MJPEG_FINISHED);
    assert_reference_sequence(12); assert(callbacks==12 && !status.presentation_drops && !status.dropped_frames);
    assert(times[1]-times[0]>=200000); destroy(h);
    puts("PASS decode-ahead prepares RGB before its PTS; pause retains that exact P picture, resume publishes it once with every reference packet decoded once");

    reset(); h=start(path); wait_converted(h,12); assert(callbacks==11);
    assert(aether_mp4_player_set_paused(h,true)==ESP_OK); wait_state(h,AETHER_MJPEG_PAUSED);
    delay(100); assert(callbacks==11 && decoded_count==12);
    assert(aether_mp4_player_set_paused(h,false)==ESP_OK); wait_frames(12);
    status=wait_state(h,AETHER_MJPEG_FINISHED);
    assert(esp_timer_get_time()-times[11]>=65000 && status.position_ms==1000);
    assert_reference_sequence(12); assert(!status.presentation_drops); destroy(h);
    puts("PASS final prepared picture survives pause/resume exactly once and remains visible for its declared duration before EOF cleanup");

    reset(); h=start(path); wait_converted(h,2); assert(callbacks==1);
    assert(aether_mp4_player_set_paused(h,true)==ESP_OK); wait_state(h,AETHER_MJPEG_PAUSED);
    aether_mp4_player_release_frame(h); assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK);
    assert(callbacks==1 && decoded_count==2); destroy(h);
    puts("PASS STOP/release cancels a prepared paused image without publishing it or feeding its P packet twice");

    reset(); h=start(path); wait_converted(h,2); assert(callbacks==1);
    assert(aether_mp4_player_set_paused(h,true)==ESP_OK); wait_state(h,AETHER_MJPEG_PAUSED);
    assert(seek_reset_retry(h,750)==ESP_OK); wait_seek(h); wait_frames(2);
    assert(callback_pts[1]==750 && callbacks==2 && consumer_resets==1 && pending_picture==751);
    assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK); destroy(h);
    puts("PASS seek invalidates a prepared old-generation RGB and publishes only the forced paused target preview");

    reset(); h=start(path); wait_converted(h,2); block_publish_take=true;
    const int64_t until=esp_timer_get_time()+2000000;
    while(!publish_waiting && esp_timer_get_time()<until) delay(1);
    assert(publish_waiting && callbacks==1);
    assert(aether_mp4_player_set_paused(h,true)==ESP_OK); block_publish_take=false;
    wait_state(h,AETHER_MJPEG_PAUSED); delay(80); assert(callbacks==1 && decoded_count==2);
    assert(seek_reset_retry(h,500)==ESP_OK); wait_seek(h); wait_frames(2);
    assert(callback_pts[1]==500 && callbacks==2);
    assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK); destroy(h);
    puts("PASS pause arriving during publication-gate wait releases the gate while retaining RGB; paused seek preview remains available");

    reset(); h=start(path); wait_converted(h,2); block_publish_take=true;
    const int64_t gate_deadline=esp_timer_get_time()+2000000;
    while(!publish_waiting && esp_timer_get_time()<gate_deadline) delay(1);
    assert(publish_waiting && callbacks==1); delay(200); block_publish_take=false;
    wait_frames(2); assert(callback_pts[1]==83);
    assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK); destroy(h);
    puts("PASS an already-converted latest picture still publishes after a delayed gate crosses its sample end instead of extending the older displayed frame");

    reset(); snprintf(path,sizeof path,"%s/baseline-gop-aac.mp4",root); block_aac_decode=true;
    h=start(path); wait_converted(h,2); assert(callbacks==1);
    fail_aac=true; block_aac_decode=false; status=wait_state(h,AETHER_MJPEG_ERROR);
    assert(status.audio_error!=ESP_OK && callbacks==1); destroy(h);
    puts("PASS AAC failure while a prepared picture waits on the common clock terminates both workers instead of retrying the private wait forever");

    reset(); snprintf(path,sizeof path,"%s/baseline-long-gop.mp4",root);
    h264_delay_ms=60; conversion_delay_ms=35; h=start(path);
    const int64_t finish_deadline=esp_timer_get_time()+10000000;
    do { assert(aether_mp4_player_get_status(h,&status)==ESP_OK); if(status.state==AETHER_MJPEG_FINISHED) break; delay(2); }
    while(esp_timer_get_time()<finish_deadline);
    assert(status.state==AETHER_MJPEG_FINISHED && status.duration_ms==5000);
    assert_reference_sequence(60);
    assert(status.presentation_drops && !status.dropped_frames && !status.lag_rebuilds && !status.lag_skip_burst_max);
    assert(status.h264_output_frames==60 && status.converted_frames<60 && callbacks<60);
    assert(status.converted_frames==callbacks && status.late_frames>1);
    assert(callbacks+status.presentation_drops==60 && callback_pts[0]==0);
    assert(status.h264_total_us>=60ULL*55000 && status.convert_total_us>=status.converted_frames*30000ULL);
    destroy(h);
    puts("PASS60ms H264 plus35ms conversion exceeds83ms display budget: selective stale display skips bound lag without compressed skips/rebuilds, all60 P-chain packets decode exactly once");
}
static void test_seek_clock(const char *root)
{
    char path[512]; snprintf(path,sizeof path,"%s/baseline-gop-aac.mp4",root);
    reset(); aether_mp4_player_handle_t h=start(path); wait_frames(2);
    slow_next_audio_start(); assert(seek_retry(h,250)==ESP_OK); wait_seek(h); wait_picture_pts(250); wait_audio_start_delay();
    const unsigned preview=callbacks; assert(callback_pts[preview-1]==250);
    delay(200); assert(callbacks==preview);
    aether_mp4_status_t status; assert(aether_mp4_player_get_status(h,&status)==ESP_OK);
    assert(status.position_ms==250 && !status.seeking);
    wait_frames(preview+2);
    assert(times[preview+1]-times[preview]>50000);
    status=wait_state(h,AETHER_MJPEG_FINISHED);
    assert(!status.dropped_frames && status.audio_clock_wait_max_us>=600000);
    destroy(h);
    puts("PASS650ms codec restart after seek holds target picture until actual PCM, then resumes paced video without IDR-drop recovery or permanent A/V startup debt");

    reset(); h=start(path); wait_frames(2);
    assert(aether_mp4_player_set_paused(h,true)==ESP_OK); wait_state(h,AETHER_MJPEG_PAUSED); delay(40);
    assert(seek_retry(h,500)==ESP_OK); wait_seek(h); wait_picture_pts(500); const unsigned paused_preview=callbacks;
    assert(callback_pts[paused_preview-1]==500 && !audio_on);
    slow_next_audio_start(); assert(aether_mp4_player_set_paused(h,false)==ESP_OK); wait_audio_start_delay();
    delay(150); assert(callbacks==paused_preview);
    assert(aether_mp4_player_stop_wait(h,40)==ESP_ERR_TIMEOUT && leases && live_heap);
    wait_state(h,AETHER_MJPEG_STOPPED); assert(callbacks==paused_preview); destroy(h);
    puts("PASS paused seek completes its preview before audio; slow resume does not advance video, and STOP retains/drains the in-flight startup safely");

    reset(); h=start(path); wait_frames(2); slow_next_audio_start();
    assert(seek_retry(h,250)==ESP_OK); wait_seek(h); wait_picture_pts(250); wait_audio_start_delay();
    const unsigned first_preview=callbacks;
    assert(seek_retry(h,750)==ESP_OK); wait_seek(h); wait_picture_pts(750); wait_frames(first_preview+1);
    assert(callback_pts[first_preview]==750);
    assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK); destroy(h);
    puts("PASS newer seek during delayed codec start cancels old clock commit and never publishes an old-generation picture after the newer command");

    reset(); snprintf(path,sizeof path,"%s/baseline-short-audio.mp4",root); h=start(path); wait_frames(2);
    assert(seek_retry(h,750)==ESP_OK); wait_seek(h); status=wait_state(h,AETHER_MJPEG_FINISHED);
    assert(status.position_ms==1000 && !status.last_error); destroy(h);
    puts("PASS seek past a shorter audio track drains all-trimmed AAC and releases the private clock gate without waiting forever for nonexistent PCM");

    reset(); snprintf(path,sizeof path,"%s/baseline-gop-aac.mp4",root); h=start(path); wait_frames(2);
    fail_audio_stop=true; assert(aether_mp4_player_set_paused(h,true)==ESP_OK); delay(50);
    assert(audio_on && leases && aac_decoders);
    assert(aether_mp4_player_stop_wait(h,40)==ESP_ERR_TIMEOUT);
    assert(aether_mp4_player_play(h,path,frame,NULL)==ESP_ERR_INVALID_STATE);
    fail_audio_stop=false; wait_state(h,AETHER_MJPEG_STOPPED); destroy(h);
    puts("PASS failed pause-time codec close keeps its ownership flag and child alive until a verified close, preventing leaked DMA/codec ownership on later playback");
}
static void test_seek_io(const char *root)
{
    char path[512]; snprintf(path,sizeof path,"%s/baseline-padded-gop.mp4",root);
    for(unsigned oom=0;oom<2;++oom) {
        reset(); fail_idr_cache=oom; aether_mp4_player_handle_t h=start(path); wait_frames(2);
        assert(aether_mp4_player_set_paused(h,true)==ESP_OK); wait_state(h,AETHER_MJPEG_PAUSED); delay(40);
        big_reads=probe_reads=0;
        unsigned before=callbacks;
        assert(seek_retry(h,750)==ESP_OK); wait_seek(h); wait_frames(before+1);
        if(big_reads!=4 || probe_reads<4 || callback_pts[callbacks-1]!=750)
            fprintf(stderr,"cold seek reads=%u probes=%u pts=%u oom=%u\n",(unsigned)big_reads,(unsigned)probe_reads,callback_pts[callbacks-1],oom);
        assert(big_reads==4 && probe_reads>=4 && callback_pts[callbacks-1]==750);
        big_reads=probe_reads=0;
        before=callbacks;
        assert(seek_retry(h,750)==ESP_OK); wait_seek(h); wait_frames(before+1);
        assert(big_reads==4 && (oom ? probe_reads>=4 : probe_reads==0));
        assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK); destroy(h);
    }
    puts("PASS cold seek inspects NAL headers instead of rereading64KB packets; warm bitmap avoids probe IO, optional-cache OOM retains exact GOP/preview semantics");

    reset(); snprintf(path,sizeof path,"%s/invalid-seek-nal.mp4",root);
    aether_mp4_player_handle_t h=start(path); wait_frames(2);
    assert(aether_mp4_player_set_paused(h,true)==ESP_OK); wait_state(h,AETHER_MJPEG_PAUSED); delay(40);
    const unsigned before=callbacks;
    assert(seek_retry(h,750)==ESP_OK);
    aether_mp4_status_t status=wait_state(h,AETHER_MJPEG_ERROR);
    assert(status.last_error==ESP_ERR_INVALID_SIZE && strstr(status.detail,"seek preceding IDR scan") && callbacks==before);
    destroy(h);
    puts("PASS malformed seek-probe NAL length fails before decoder/publication and releases all metadata, cache, frame and media ownership");
}
static void wait_aac_resets(unsigned count)
{
    const int64_t until=esp_timer_get_time()+2000000;
    while(aac_resets<count && esp_timer_get_time()<until) delay(1);
    assert(aac_resets>=count);
}
static void test_retained_aac(const char *root)
{
    char path[512]; snprintf(path,sizeof path,"%s/baseline-primed-aac.mp4",root);
    reset(); aether_mp4_player_handle_t h=start(path); wait_frames(2);
    assert(aac_opens==1 && aac_decoders==1);
    assert(aether_mp4_player_set_paused(h,true)==ESP_OK); wait_state(h,AETHER_MJPEG_PAUSED); delay(40);
    const unsigned targets[]={750,0,500,0};
    for(unsigned i=0;i<sizeof targets/sizeof targets[0];++i) {
        const unsigned old_resets=aac_resets;
        assert(seek_retry(h,targets[i])==ESP_OK); wait_seek(h); wait_picture_pts(targets[i]); wait_aac_resets(old_resets+1);
        assert(aac_opens==1 && aac_decoders==1 && !aac_closes && !audio_on);
        assert(task_calls==2);
        const unsigned completed_resets=aac_resets;
        xTaskNotifyGive(tasks[1]); xTaskNotifyGive(tasks[1]); delay(10);
        assert(aac_resets==completed_resets && task_calls==2);
    }
    const uint64_t before=audio_frames;
    assert(aether_mp4_player_set_paused(h,false)==ESP_OK); wait_state(h,AETHER_MJPEG_FINISHED);
    assert(audio_frames-before==49152-1024); /* Exact edit, not rounded -21333us priming. */
    assert(aac_opens==1 && aac_closes==1); destroy(h);
    puts("PASS whole video session retains one AAC worker/SDK instance across paused750/0/500/0 seeks, duplicate notifications cannot replay an epoch, exact1024-frame priming restored at zero; final close exactly once");

    reset(); snprintf(path,sizeof path,"%s/baseline-short-audio.mp4",root); h=start(path); wait_frames(7); delay(30);
    assert(!audio_on && aac_decoders==1 && aac_opens==1 && !aac_closes);
    assert(aether_mp4_player_set_paused(h,true)==ESP_OK); wait_state(h,AETHER_MJPEG_PAUSED); delay(30);
    const unsigned old_resets=aac_resets;
    assert(seek_retry(h,0)==ESP_OK); wait_seek(h); wait_picture_pts(0); wait_aac_resets(old_resets+1);
    const uint64_t before_eof_replay=audio_frames;
    assert(aether_mp4_player_set_paused(h,false)==ESP_OK); wait_state(h,AETHER_MJPEG_FINISHED);
    assert(audio_frames-before_eof_replay==12288 && aac_opens==1 && aac_closes==1); destroy(h);
    puts("PASS early audio EOF retains its decoder until video-session end; seek-zero resets finished state and replays every PCM frame without reopening private work memory");

    reset(); snprintf(path,sizeof path,"%s/baseline-gop-aac.mp4",root); h=start(path); wait_frames(2);
    block_aac_decode=true;
    int64_t until=esp_timer_get_time()+1000000; while(!aac_decoding && esp_timer_get_time()<until) delay(1); assert(aac_decoding);
    const unsigned blocked_resets=aac_resets;
    assert(seek_retry(h,750)==ESP_OK); assert(seek_retry(h,250)==ESP_OK); delay(30);
    assert(aac_decoders==1 && aac_opens==1 && !aac_closes && aac_resets==blocked_resets && leases);
    block_aac_decode=false; wait_seek(h); wait_picture_pts(250); wait_aac_resets(blocked_resets+1);
    assert(aac_opens==1 && !aac_closes);
    assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK); assert(aac_closes==1); destroy(h);
    puts("PASS replacement seeks wait for a blocked old AAC decode before resetting the retained handle; newest epoch wins without concurrent reset or premature delete");

    reset(); h=start(path); wait_frames(2); fail_aac_reset=true;
    assert(seek_retry(h,0)==ESP_OK); aether_mp4_status_t status=wait_state(h,AETHER_MJPEG_ERROR);
    assert(status.audio_error!=ESP_OK && aac_opens==1 && aac_closes==1); destroy(h);
    puts("PASS retained SDK reset failure at seek-zero terminates both workers and closes the retained instance exactly once");

    reset(); h=start(path); wait_frames(2); fail_task=(int)task_calls+1;
    assert(seek_retry(h,750)==ESP_OK); wait_seek(h); wait_picture_pts(750); wait_state(h,AETHER_MJPEG_FINISHED);
    assert(task_calls==2 && aac_opens==1 && aac_closes==1); destroy(h);
    puts("PASS seek reuses its parked AAC worker even when another task allocation would fail; no new stack/TCB is requested");

    reset(); fail_task=2; h=start(path); status=wait_state(h,AETHER_MJPEG_ERROR);
    assert(status.last_error==ESP_ERR_NO_MEM && task_calls==2 && !aac_opens && !aac_closes); destroy(h);
    puts("PASS initial persistent-AAC task allocation failure unwinds without a phantom worker or decoder ownership");

    reset(); snprintf(path,sizeof path,"%s/baseline-short-audio.mp4",root); h=start(path); wait_frames(7); delay(30);
    assert(!audio_on && aac_decoders==1); block_worker_exit=true;
    assert(aether_mp4_player_stop_wait(h,40)==ESP_ERR_TIMEOUT);
    assert(aac_decoders==1 && !aac_closes && leases && live_heap);
    block_worker_exit=false; wait_state(h,AETHER_MJPEG_STOPPED); assert(aac_closes==1); destroy(h);
    puts("PASS segment EOF acknowledgment cannot free a parked worker session; parent waits for a distinct final worker-exit barrier before decoder/file-lease cleanup");
}
static void test_seek(const char *root)
{
    char path[512]; snprintf(path,sizeof path,"%s/baseline-gop-aac.mp4",root);
    reset(); aether_mp4_player_handle_t h=start(path); wait_frames(2);
    assert(aether_mp4_player_set_paused(h,true)==ESP_OK); wait_state(h,AETHER_MJPEG_PAUSED); delay(40);
    const unsigned before=callbacks, units=decoded_count;
    assert(seek_reset_retry(h,750)==ESP_OK); wait_seek(h); wait_frames(before+1);
    aether_mp4_status_t status; assert(aether_mp4_player_get_status(h,&status)==ESP_OK);
    assert(status.state==AETHER_MJPEG_PAUSED && status.position_ms==750 && !audio_on);
    assert(callback_pts[before]==750 && callbacks==before+1 && consumer_resets==1 && pending_picture==751);
    assert(decoded_units[units].type==5 && decoded_units[units].pts==500);
    for(unsigned i=units+1;i<decoded_count;++i) assert(decoded_units[i].decoder==decoded_units[units].decoder);
    const unsigned previews=callbacks; const uint64_t base=audio_frames; delay(80); assert(callbacks==previews && audio_frames==base);
    assert(aether_mp4_player_set_paused(h,false)==ESP_OK); wait_state(h,AETHER_MJPEG_FINISHED);
    /* 48*1024 source frames, resumed exactly at 750*48 output frames. */
    if(audio_frames-base!=49152-36000) fprintf(stderr,"seek audio frames=%llu expected=%u base=%llu total=%llu\n",(unsigned long long)(audio_frames-base),49152-36000,(unsigned long long)base,(unsigned long long)audio_frames);
    assert(audio_frames-base==49152-36000);
    destroy(h);
    puts("PASS seek real GOP samples: reset starts at preceding IDR, decodes P references, paused target preview only, AAC overlap/preroll trims exact 48k timeline");

    reset(); snprintf(path,sizeof path,"%s/baseline-gop.mp4",root); h=start(path); wait_frames(1);
    block_decode=true; while(!decode_block_entered) delay(1);
    const unsigned old=callbacks;
    assert(seek_retry(h,750)==ESP_OK); assert(seek_retry(h,250)==ESP_OK);
    block_decode=false; wait_seek(h); wait_frames(old+1);
    assert(callback_pts[old]==250); /* No old decoded/earlier-seek frame escaped. */
    assert(aether_mp4_player_stop_wait(h,2000)==ESP_OK); destroy(h);
    puts("PASS rapid seeks coalesce to newest target and accepted seek suppresses a previously in-flight decoded frame");

    reset(); h=start(path); wait_frames(2);
    /* Decode-ahead starts up to one sample earlier. Keep a fault larger than
     * that lead plus the unchanged500ms compressed-recovery threshold. */
    block_decode=true; while(!decode_block_entered) delay(1); delay(800); block_decode=false;
    status=wait_state(h,AETHER_MJPEG_FINISHED);
    assert(status.dropped_frames && decoder_serial>=2 && status.lag_rebuilds && status.lag_skip_burst_max);
    assert(status.dropped_frames>=status.lag_skip_burst_max && status.h264_max_us>=750000);
    bool lag_idr=false;
    for(unsigned i=0;i<decoded_count;++i) if(decoded_units[i].decoder>1 && decoded_units[i].type==5 && decoded_units[i].pts==500) lag_idr=true;
    assert(lag_idr); destroy(h);
    puts("PASS lag decoder rebuild re-primes fresh SPS/PPS without overwriting its pending IDR access unit");

    reset(); block_callback=true; h=start(path); while(!callback_inside) delay(1);
    const int64_t command=esp_timer_get_time();
    assert(aether_mp4_player_seek_with_reset(h,500,clear_consumer,&consumer_resets)==ESP_ERR_INVALID_STATE && esp_timer_get_time()-command<10000);
    assert(!consumer_resets);
    block_callback=false; wait_frames(1); assert(seek_retry(h,500)==ESP_OK);
    assert(aether_mp4_player_stop(h)==ESP_OK); wait_state(h,AETHER_MJPEG_STOPPED); destroy(h);
    puts("PASS frame-publication contention returns immediately for GUI retry; STOP cancels an accepted seek without leaking the session lease");
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    assert(argc==2); char video[512],av[512],bad[512];
    snprintf(video,sizeof(video),"%s/baseline.mp4",argv[1]); snprintf(av,sizeof(av),"%s/baseline-aac.mp4",argv[1]);
    reset(); aether_mp4_player_handle_t h=start(video); aether_mp4_status_t status=wait_state(h,AETHER_MJPEG_FINISHED);
    assert(callbacks==12 && status.frame_number==12 && status.duration_ms==1000 && status.position_ms==1000 && !status.has_audio);
    assert(status.width==32 && status.source_fps_x100==1200 && !status.seek_supported && times[11]-times[0]>800000);
    assert(status.h264_output_frames==12 && status.converted_frames==12);
    assert(video_task_core==EXPECTED_VIDEO_CORE && audio_task_core==-2);
    assert(status.h264_max_us>=status.h264_us && status.convert_max_us>=status.convert_us);
    assert(status.read_max_us>=status.read_us && status.decode_max_us>=status.decode_us && status.callback_max_us>=status.callback_us);
    destroy(h);
    assert(log_count==3 && strstr(diagnostic_log,"I start path=") && strstr(diagnostic_log,"video_object=0x21 profile=66 constraints=0xc0 level=12") &&
        strstr(diagnostic_log,"display=32x32 coded=32x32") && strstr(diagnostic_log,"fps=12.00") && strstr(diagnostic_log,"I finished path=") && !strstr(diagnostic_log,"E "));
    puts("PASS production MP4 worker: real demux/Annex-B/converted frames/native cadence/EOF/lease cleanup; exactly three lifecycle logs, no per-frame logs");
    reset(); conversion_delay_ms=17; callback_delay_ms=3;
    h=start(video); status=wait_state(h,AETHER_MJPEG_FINISHED);
    assert(status.h264_output_frames==12 && status.converted_frames==12 && callbacks==12);
    assert(status.h264_max_us>=1000 && status.decode_us>=status.h264_us+status.convert_us);
    assert(status.convert_us>=15000 && status.convert_max_us>=status.convert_us);
    assert(status.callback_us>=2000 && status.callback_max_us>=status.callback_us);
    assert(status.decode_max_us>=status.convert_max_us && !status.lag_rebuilds && !status.lag_skip_burst_max);
    destroy(h);
    puts("PASS cached timing separates SDK H264 processing,17ms conversion and3ms callback costs with exact output/converted counts and unchanged playback cadence");
    reset(); h=start(av); status=wait_state(h,AETHER_MJPEG_FINISHED);
    assert(callbacks==12 && audio_writes>=48 && status.has_audio && status.audio_rate==48000 && status.audio_channels==2 && !status.audio_error);
    assert(video_task_core==EXPECTED_VIDEO_CORE && audio_task_core==0);
    assert(status.audio_write_max_us>=1000 && status.audio_decode_us<status.audio_write_max_us);
    assert(status.audio_read_max_us>=status.audio_read_us && status.audio_decode_max_us>=status.audio_decode_us);
    assert(status.audio_supply_gap_max_us>=status.audio_supply_gap_us);
    assert(status.audio_lookup_max_us>=status.audio_lookup_us && status.video_lookup_max_us>=status.video_lookup_us);
    destroy(h); assert(log_count==4 && strstr(diagnostic_log,"AAC-LC rate=48000 channels=2 ASC=2 bytes"));
    puts("PASS production MP4 with real AAC adapter/resampler: independent paced PCM output drains before session completion; source audio format logged once");
    reset(); h=start(av); wait_frames(2); assert(aether_mp4_player_set_paused(h,true)==ESP_OK);
    wait_state(h,AETHER_MJPEG_PAUSED); delay(40); unsigned frames=callbacks,writes=audio_writes; delay(80);
    assert(callbacks==frames && audio_writes==writes && !audio_on);
    assert(aether_mp4_player_set_paused(h,false)==ESP_OK); wait_frames(frames+2);
    inserted=false; status=wait_state(h,AETHER_MJPEG_ERROR); assert(status.last_error==ESP_ERR_INVALID_STATE);
    destroy(h); puts("PASS pause halts images/audio and resumes both; hot removal cancels both workers before releasing the mount lease");
    reset(); block_decode=true; h=start(av); while(!decode_block_entered) delay(1);
    aether_mp4_player_release_frame(h); assert(aether_mp4_player_stop_wait(h,0)==ESP_ERR_TIMEOUT);
    block_decode=false; wait_state(h,AETHER_MJPEG_STOPPED); assert(!callbacks); destroy(h);
    puts("PASS stop/release during H264 work suppresses callbacks and retained-buffer use after teardown");
    reset();h=start(av);while(!audio_writes)delay(1);
    fail_audio_stop=true;aether_mp4_player_release_frame(h);
    assert(aether_mp4_player_stop_wait(h,40)==ESP_ERR_TIMEOUT);
    assert(audio_on&&leases&&aac_decoders&&live_heap);
    assert(aether_mp4_player_play(h,av,frame,NULL)==ESP_ERR_INVALID_STATE);
    unsigned retained_audio_stops=audio_stops;delay(110);assert(audio_on&&leases&&audio_stops>=retained_audio_stops);
    fail_audio_stop=false;wait_state(h,AETHER_MJPEG_STOPPED);destroy(h);
    puts("PASS failed MP4 codec close retains audio child/session/lease until a checked retry; reentry is rejected and final stop drains all ownership");

    for(unsigned fault=0;fault<8;++fault) {
        reset(); if(fault<3) fail_heap=(int)fault+1; if(fault==3) fail_decoder=1; if(fault==4) fail_decode=1;
        if(fault==5) fail_aac=1;
        if(fault==6) fail_audio=1;
        if(fault==7) fail_task=2;
        h=start(av); status=wait_state(h,AETHER_MJPEG_ERROR); assert(status.last_error!=ESP_OK && status.detail[0]); destroy(h);
        assert(strstr(diagnostic_log,"E failed path=") && strstr(diagnostic_log,"stage=") && strstr(diagnostic_log,"error=") && log_count<8);
    }
    puts("PASS metadata/frame allocation, H264 creation/decode, AAC creation and codec-start faults unwind both task lifetimes");
    reset(); snprintf(bad,sizeof(bad),"%s/high-profile.mp4",argv[1]); h=start(bad);
    status=wait_state(h,AETHER_MJPEG_ERROR); assert(status.last_error==ESP_ERR_NOT_SUPPORTED && strstr(status.detail,"High") && !callbacks); destroy(h);
    assert(strstr(diagnostic_log,"stage=MP4 demux/format validation") && strstr(diagnostic_log,"profile=100 constraints=0xc0 level=12"));
    puts("PASS unsupported MP4 video exposes a real format reason and never pretends playback succeeded");
    reset(); audio_start_invalid=true; h=start(av); status=wait_state(h,AETHER_MJPEG_ERROR);
    assert(status.last_error==ESP_ERR_INVALID_STATE && status.audio_error==ESP_ERR_INVALID_STATE && strstr(status.detail,"audio output start"));
    destroy(h); assert(strstr(diagnostic_log,"E audio failed path=") && strstr(diagnostic_log,"stage=audio output start"));
    reset(); snprintf(bad,sizeof bad,"%s/missing-video.mp4",argv[1]); h=start(bad); status=wait_state(h,AETHER_MJPEG_ERROR); destroy(h);
    assert(status.last_error==ESP_ERR_NOT_FOUND && strstr(status.detail,"video file open") && strstr(diagnostic_log,"stage=video file open"));
    reset(); fail_task=1; assert(aether_mp4_player_new((void*)1,(void*)1,&h)==ESP_OK);
    assert(aether_mp4_player_play(h,video,frame,NULL)==ESP_ERR_NO_MEM);
    status=wait_state(h,AETHER_MJPEG_ERROR); assert(strstr(status.detail,"24KiB stack")); destroy(h);
    assert(strstr(diagnostic_log,"stage=video_task_create"));
    puts("PASS codec INVALID_STATE remains an audio failure; missing file and task-allocation errors carry actionable stages");
    test_hp_lifecycle(argv[1]);
    test_prepared_video(argv[1]);
    test_seek(argv[1]);
    test_seek_clock(argv[1]);
    test_seek_io(argv[1]);
    test_retained_aac(argv[1]);
    const char *audio_paths[]={"audio-only.m4a","high-profile-aac.mp4","iso5-fragmented.mp3"};
    for(unsigned test=0;test<3;++test) {
        reset(); snprintf(bad,sizeof(bad),"%s/%s",argv[1],audio_paths[test]);
        FILE *file=fopen(bad,"rb"); assert(file); aether_mp4_audio_handle_t stream=NULL; char detail[112]={0};
        assert(aether_mp4_audio_open(file,NULL,NULL,&stream,detail,sizeof(detail))==ESP_OK);
        aether_mp4_audio_info_t info; assert(aether_mp4_audio_get_info(stream,&info)==ESP_OK);
        assert(info.sample_rate==48000 && info.channels==2 && info.duration_ms==1024 && info.object_type==0x40 && info.asc_size==2);
        int16_t pcm[137*2]; size_t total=0,count; esp_err_t error=ESP_OK;
        while((count=aether_mp4_audio_read(stream,pcm,137,&error))!=0) {
            for(size_t i=0;i<count*2;++i) assert(!pcm[i]);
            total+=count;
        }
        assert(error==ESP_OK && total==49152);
        assert(aether_mp4_audio_get_info(stream,&info)==ESP_OK && info.finished && info.position_ms==1024);
        assert(aether_mp4_audio_seek(stream,500)==ESP_OK); total=0;
        while((count=aether_mp4_audio_read(stream,pcm,137,&error))!=0) total+=count;
        assert(error==ESP_OK && total==49152-24000);
        assert(aether_mp4_audio_seek(stream,0)==ESP_OK);
        count=aether_mp4_audio_read(stream,pcm,137,&error); assert(count==137 && error==ESP_OK);
        aether_mp4_audio_close(stream); assert(fseek(file,0,SEEK_SET)==0); fclose(file);
        assert(!aac_decoders && !live_heap);
    }
    puts("PASS M4A, High-profile video AAC and actual iso5 fMP4 (.mp3 name): partial PCM reads/seek preroll/EOF; borrowed FILE survives close");
    const char *bad_audio[]={"iso5-gap.m4a","iso5-size.m4a","iso5-offset.m4a","iso5-base.m4a","iso5-header.m4a","iso5-cross.m4a","iso5-wrap.m4a"};
    for(unsigned test=0;test<sizeof(bad_audio)/sizeof(bad_audio[0]);++test) {
        reset(); snprintf(bad,sizeof(bad),"%s/%s",argv[1],bad_audio[test]); FILE *file=fopen(bad,"rb"); assert(file);
        aether_mp4_audio_handle_t stream=NULL; char detail[112]={0};
        assert(aether_mp4_audio_open(file,NULL,NULL,&stream,detail,sizeof(detail))!=ESP_OK && !stream && detail[0]);
        fclose(file); assert(!aac_decoders && !live_heap);
    }
    puts("PASS fMP4 gap/oversized AU/negative offset/INT64 base/header pointer/cross-mdat/UINT64 timeline wrap reject without leaks");
    return 0;
}
