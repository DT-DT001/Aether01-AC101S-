#include "aether_recorder.h"
#include "aether_recorder_denoise.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define ORIGINAL_RATE 48000U
#define RING_BYTES (256U * 1024U)
#define RX_BYTES 4096U
#define WRITE_BYTES (8U * 1024U)
#define STDIO_BYTES (16U * 1024U)
/* ESP-IDF's signed 32-bit off_t/long, RIFF and FAT32 all remain in range. */
#define MAX_DATA_BYTES (((uint64_t)INT32_MAX - 44U) & ~UINT64_C(1))
#define DIRECTORY AETHER_STORAGE_MOUNT_POINT "/Recordings"

_Static_assert(AETHER_DENOISE_CAPTURE_FRAMES * 4U <= RX_BYTES,
               "the receive buffer must hold one denoise hop");
_Static_assert((RING_BYTES & (RING_BYTES - 1U)) == 0, "ring indexing requires a power of two");

static const char *TAG = "recorder";

struct aether_recorder {
    ac101s_audio_handle_t audio;
    aether_storage_handle_t storage;
    portMUX_TYPE lock;
    SemaphoreHandle_t commands;
    atomic_bool stop, pause, screen_paused;
    atomic_int error;
    bool destroying;
    aether_recorder_mode_t requested_mode;
    aether_recorder_status_t status;
};
typedef struct {
    aether_recorder_handle_t h;
    uint8_t *ring, *rx, *output, *stdio_buffer;
    aether_recorder_denoise_t *denoise;
    uint32_t sample_rate;
    atomic_uint produced, consumed;
    atomic_bool producer_done;
} session_t;
static _Atomic(aether_recorder_handle_t) s_default;

static void text(char *out,const char *in,size_t capacity)
{ size_t n=strlen(in);if(n>=capacity)n=capacity-1;memcpy(out,in,n);out[n]=0; }
static void fail(aether_recorder_handle_t h,esp_err_t error,const char *detail)
{
    int expected=ESP_OK;
    if(atomic_compare_exchange_strong(&h->error,&expected,error)) {
        portENTER_CRITICAL(&h->lock);
        h->status.last_error=error;text(h->status.detail,detail,sizeof(h->status.detail));
        portEXIT_CRITICAL(&h->lock);
    }
    atomic_store(&h->stop,true);
}
static void state(aether_recorder_handle_t h,aether_recorder_state_t value)
{ portENTER_CRITICAL(&h->lock);h->status.state=value;portEXIT_CRITICAL(&h->lock); }
static void progress(session_t *session, uint64_t bytes)
{
    aether_recorder_handle_t h = session->h;
    portENTER_CRITICAL(&h->lock);
    h->status.data_bytes = bytes;
    h->status.duration_ms = bytes * 1000U / (session->sample_rate * 2U);
    portEXIT_CRITICAL(&h->lock);
}
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){put16(p,(uint16_t)v);put16(p+2,(uint16_t)(v>>16));}
static void wav_header(uint8_t out[44],uint32_t bytes,uint32_t sample_rate)
{
    memset(out,0,44);memcpy(out,"RIFF",4);put32(out+4,bytes+36);memcpy(out+8,"WAVEfmt ",8);
    put32(out+16,16);put16(out+20,1);put16(out+22,1);put32(out+24,sample_rate);
    put32(out+28,sample_rate*2);put16(out+32,2);put16(out+34,16);memcpy(out+36,"data",4);put32(out+40,bytes);
}
static bool checkpoint(FILE *file,uint64_t bytes,uint32_t sample_rate)
{
    uint8_t header[44];wav_header(header,(uint32_t)bytes,sample_rate);
    /* Serialize buffered data before seeking to the header, then return to
     * its exact end. Only this task ever accesses the FILE. */
    if(fflush(file)||fseek(file,0,SEEK_SET)||fwrite(header,1,44,file)!=44||
       fflush(file)||fsync(fileno(file))||fseek(file,(long)(44+bytes),SEEK_SET)) return false;
    return true;
}
static FILE *create_file(aether_recorder_handle_t h,uint8_t *buffer,char path[192])
{
    if(mkdir(DIRECTORY,0777) && errno!=EEXIST) return NULL;
    struct stat directory;
    if(stat(DIRECTORY,&directory)||!S_ISDIR(directory.st_mode)) return NULL;
    const unsigned long long stamp=(unsigned long long)(esp_timer_get_time()/1000);
    for(unsigned suffix=0;suffix<10000;++suffix) {
        const int n=snprintf(path,192,DIRECTORY "/REC_%010llu_%04u.wav",stamp,suffix);
        if(n<0||n>=192)return NULL;
        int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0666);
        if(fd<0){if(errno==EEXIST)continue;return NULL;}
        FILE *file=fdopen(fd,"w+b");
        if(!file){close(fd);unlink(path);return NULL;}
        if(setvbuf(file,(char*)buffer,_IOFBF,STDIO_BYTES)){fclose(file);unlink(path);return NULL;}
        portENTER_CRITICAL(&h->lock);text(h->status.path,path,sizeof(h->status.path));portEXIT_CRITICAL(&h->lock);
        return file;
    }
    errno=EEXIST;return NULL;
}

static BaseType_t create_task(TaskFunction_t task, const char *name, void *arg,
                              UBaseType_t priority, bool denoise)
{
#ifdef AETHER_RECORDER_HAS_NSNET3
    /* S31's PIE instructions are available on HP core 1 only. Keep creation,
     * inference and destruction of the ESP-SR instance on that core. */
    if (denoise) return xTaskCreatePinnedToCore(task, name, 6144, arg, priority, NULL, 1);
#else
    (void)denoise;
#endif
    return xTaskCreate(task, name, 4096, arg, priority, NULL);
}

static void capture_task(void *argument)
{
    session_t *s=argument;aether_recorder_handle_t h=s->h;
    const size_t rx_bytes = s->denoise ? AETHER_DENOISE_CAPTURE_FRAMES * 4U : RX_BYTES;
    const uint32_t samples_per_block = s->denoise ? AETHER_DENOISE_SAMPLES : RX_BYTES / 4U;
    const uint32_t bytes = samples_per_block * 2U;
    bool capture_attempted=false;
    bool denoise_logged = false;
    while(!atomic_load(&h->stop)) {
        if(!aether_storage_card_inserted(h->storage)){fail(h,ESP_ERR_INVALID_STATE,"TF卡已拔出，正在保存可恢复部分");break;}
        if (atomic_load(&h->screen_paused)) {
            if (capture_attempted) {
                const esp_err_t error = ac101s_audio_capture_stop(h->audio);
                if (error != ESP_OK) { fail(h, error, "息屏暂停麦克风失败"); break; }
                capture_attempted = false;
            }
            portENTER_CRITICAL(&h->lock);
            h->status.peak = 0;
            if (!atomic_load(&h->stop)) h->status.state = AETHER_RECORDER_PAUSED;
            portEXIT_CRITICAL(&h->lock);
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }
        if (!capture_attempted) {
            capture_attempted = true;
            const esp_err_t error = ac101s_audio_capture_start(h->audio, 24.0f);
            if (error != ESP_OK) { fail(h, error, "无法启动麦克风采集"); break; }
        }
        const esp_err_t error=ac101s_audio_capture_read(h->audio,s->rx,rx_bytes);
        if(error!=ESP_OK){fail(h,error,"麦克风采集失败，录音已停止");break;}
        if(atomic_load(&h->stop))break;
        const bool paused=atomic_load(&h->pause) || atomic_load(&h->screen_paused);
        uint16_t peak=0;
        const uint32_t produced=atomic_load_explicit(&s->produced,memory_order_relaxed);
        const uint32_t consumed=atomic_load_explicit(&s->consumed,memory_order_acquire);
        if (!paused && produced - consumed > RING_BYTES - bytes) {
            ESP_LOGW(TAG, "TF write backlog exhausted at %u Hz", (unsigned)s->sample_rate);
            fail(h,ESP_ERR_NO_MEM,"TF卡写入过慢，录音队列已满");
            break;
        }
        if(!paused) {
            const int16_t *samples=(const int16_t*)s->rx;
            const int16_t *clean = NULL;
            if (s->denoise) {
                const int64_t started = denoise_logged ? 0 : esp_timer_get_time();
                clean = aether_recorder_denoise_process(s->denoise, samples);
                if (!clean) {
                    fail(h, ESP_FAIL, "AI降噪中断，正在保存已录部分");
                    break;
                }
                if (!denoise_logged) {
                    ESP_LOGI(TAG, "NSNet3 processed first frame in %u us",
                             (unsigned)(esp_timer_get_time() - started));
                    denoise_logged = true;
                }
            }
            for (uint32_t i = 0; i < samples_per_block; ++i) {
                /* Preserve both USB microphone channels when saving mono.
                 * The board ADC duplicates L/R, so its level is unchanged.
                 * Widen before addition to avoid PCM16 overflow. */
                const int16_t sample = clean ? clean[i] :
                    (int16_t)(((int32_t)samples[i*2]+samples[i*2+1])/2);
                const uint16_t level=sample<0?(uint16_t)-(int32_t)sample:(uint16_t)sample;
                if(level>peak)peak=level;
                put16(s->ring+((produced+i*2)&(RING_BYTES-1)),(uint16_t)sample);
            }
            atomic_store_explicit(&s->produced,produced+bytes,memory_order_release);
        }
        portENTER_CRITICAL(&h->lock);
        h->status.peak=peak;
        if(!atomic_load(&h->stop)) h->status.state=paused?AETHER_RECORDER_PAUSED:AETHER_RECORDER_RECORDING;
        portEXIT_CRITICAL(&h->lock);
        /* Normally capture_read blocks at the hardware sample rate. Retain a
         * scheduler opportunity if a prefilled RX queue completes immediately. */
        taskYIELD();
    }
    if(capture_attempted) {
        /* A partially failed start/close may retain the codec reservation.
         * Keep this owner alive until the capture API confirms release. */
        for(;;) {
            esp_err_t stopped=ac101s_audio_capture_stop(h->audio);
            if(stopped==ESP_OK)break;
            fail(h,stopped,"关闭麦克风失败，正在重试");
            state(h,AETHER_RECORDER_SAVING);
            vTaskDelay(pdMS_TO_TICKS(100)?pdMS_TO_TICKS(100):1);
        }
    }
    /* No session/handle access after publishing done: the writer owns free. */
    atomic_store_explicit(&s->producer_done,true,memory_order_release);
    vTaskDelete(NULL);
}
static bool repair_file(const char *path,uint64_t attempted,uint32_t sample_rate,uint64_t *saved)
{
    *saved=0;
    int fd=open(path,O_RDWR);if(fd<0)return false;
    struct stat info;bool ok=fstat(fd,&info)==0;
    uint64_t bytes=ok&&info.st_size>44?(uint64_t)info.st_size-44:0;
    if(bytes>attempted)bytes=attempted;
    if(bytes>MAX_DATA_BYTES)bytes=MAX_DATA_BYTES;
    bytes&=~UINT64_C(1);
    uint8_t header[44];wav_header(header,(uint32_t)bytes,sample_rate);
    if(ok)ok=ftruncate(fd,(off_t)(44+bytes))==0;
    if(ok)ok=lseek(fd,0,SEEK_SET)==0;
    size_t offset=0;
    while(ok&&offset<44){ssize_t n=write(fd,header+offset,44-offset);if(n<0&&errno==EINTR)continue;if(n<=0)ok=false;else offset+=(size_t)n;}
    if(fsync(fd))ok=false;
    if(close(fd))ok=false;
    *saved=bytes;return ok;
}
static void writer_task(void *argument)
{
    aether_recorder_handle_t h=argument;session_t *s=calloc(1,sizeof(*s));
    FILE *file=NULL;bool leased=false,capture_created=false;char path[192]={0};
    uint64_t accepted=0,attempted=0;int64_t checkpoint_at=esp_timer_get_time();uint64_t checkpointed=0;
    uint32_t peak_backlog = 0;
    unsigned max_write_ms = 0, max_checkpoint_ms = 0;
    if(!s){fail(h,ESP_ERR_NO_MEM,"无法分配录音控制内存");goto done;}
    s->h=h;atomic_init(&s->produced,0);atomic_init(&s->consumed,0);atomic_init(&s->producer_done,true);
    s->sample_rate = ORIGINAL_RATE;
    s->ring=heap_caps_malloc(RING_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s->rx=heap_caps_malloc(RX_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s->output=heap_caps_malloc(WRITE_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s->stdio_buffer=heap_caps_malloc(STDIO_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s->ring||!s->rx||!s->output||!s->stdio_buffer){fail(h,ESP_ERR_NO_MEM,"录音缓冲内存不足");goto done;}
    if(atomic_load(&h->stop))goto done;
    esp_err_t error=aether_storage_acquire(h->storage,0);
    if(error!=ESP_OK){fail(h,error,"TF卡尚未挂载，无法录音");goto done;}leased=true;
    if(!aether_storage_card_inserted(h->storage)){fail(h,ESP_ERR_INVALID_STATE,"TF卡已拔出");goto done;}
    if (h->requested_mode == AETHER_RECORDER_VOICE_DENOISE) {
        s->denoise = aether_recorder_denoise_open();
        if (s->denoise) s->sample_rate = AETHER_DENOISE_RATE;
        portENTER_CRITICAL(&h->lock);
        h->status.mode = s->denoise ? AETHER_RECORDER_VOICE_DENOISE : AETHER_RECORDER_ORIGINAL;
        h->status.denoise_fallback = !s->denoise;
        portEXIT_CRITICAL(&h->lock);
    }
    if (atomic_load(&h->stop)) goto done;
    file=create_file(h,s->stdio_buffer,path);
    if(!file){fail(h,errno==ENOMEM?ESP_ERR_NO_MEM:ESP_FAIL,"无法创建录音文件，请检查TF卡空间");goto done;}
    if(!checkpoint(file,0,s->sample_rate)){fail(h,ESP_FAIL,"初始化WAV文件失败");goto done;}
    atomic_store(&s->producer_done,false);
    if(create_task(capture_task,"rec_capture",s,5,s->denoise!=NULL)!=pdPASS){atomic_store(&s->producer_done,true);fail(h,ESP_ERR_NO_MEM,"无法创建录音采集任务");goto done;}
    capture_created=true;
    for(;;) {
        if(!aether_storage_card_inserted(h->storage)){fail(h,ESP_ERR_INVALID_STATE,"TF卡已拔出，正在保存可恢复部分");break;}
        const uint32_t consumed=atomic_load_explicit(&s->consumed,memory_order_relaxed);
        const uint32_t available=atomic_load_explicit(&s->produced,memory_order_acquire)-consumed;
        if (available > peak_backlog) peak_backlog = available;
        if(available) {
            size_t amount=available<WRITE_BYTES?available:WRITE_BYTES;
            if(amount>MAX_DATA_BYTES-accepted)amount=(size_t)(MAX_DATA_BYTES-accepted);
            if(!amount){fail(h,ESP_ERR_INVALID_SIZE,"已达到单个WAV文件大小上限");break;}
            size_t first=RING_BYTES-(consumed&(RING_BYTES-1));if(first>amount)first=amount;
            memcpy(s->output,s->ring+(consumed&(RING_BYTES-1)),first);
            if(first<amount)memcpy(s->output+first,s->ring,amount-first);
            atomic_store_explicit(&s->consumed,consumed+(uint32_t)amount,memory_order_release);
            attempted=accepted+amount;
            const int64_t write_started = esp_timer_get_time();
            size_t wrote=fwrite(s->output,1,amount,file);accepted+=wrote;progress(s,accepted&~UINT64_C(1));
            const unsigned write_ms = (unsigned)((esp_timer_get_time() - write_started) / 1000);
            if (write_ms > max_write_ms) max_write_ms = write_ms;
            if(wrote!=amount){fail(h,ESP_FAIL,"TF卡写入失败或空间已满");break;}
        } else if(atomic_load_explicit(&s->producer_done,memory_order_acquire))break;
        else {
            const TickType_t delay = pdMS_TO_TICKS(atomic_load(&h->screen_paused) ? 25 : 5);
            vTaskDelay(delay ? delay : 1);
        }
        if(atomic_load(&h->stop))state(h,AETHER_RECORDER_SAVING);
        const uint32_t backlog = atomic_load_explicit(&s->produced, memory_order_acquire) -
                                 atomic_load_explicit(&s->consumed, memory_order_relaxed);
        if (accepted != checkpointed && backlog <= RING_BYTES / 4 &&
            esp_timer_get_time() - checkpoint_at >= 2000000) {
            const int64_t sync_started = esp_timer_get_time();
            if(!checkpoint(file,accepted,s->sample_rate)){fail(h,ESP_FAIL,"录音检查点写入失败");break;}
            checkpointed=accepted;checkpoint_at=esp_timer_get_time();
            const unsigned sync_ms = (unsigned)((checkpoint_at - sync_started) / 1000);
            if (sync_ms > max_checkpoint_ms) max_checkpoint_ms = sync_ms;
        }
    }
done:
    atomic_store(&h->stop,true);state(h,AETHER_RECORDER_SAVING);
    if(capture_created)while(!atomic_load_explicit(&s->producer_done,memory_order_acquire))vTaskDelay(1);
    if (capture_created) {
        ESP_LOGI(TAG, "TF queue peak=%u/%u bytes, write max=%u ms, checkpoint max=%u ms",
                 (unsigned)peak_backlog, (unsigned)RING_BYTES, max_write_ms, max_checkpoint_ms);
    }
    if (s) aether_recorder_denoise_close(s->denoise);
    if(file) {
        /* Close the buffered FILE before reopening for repair: a failed flush
         * must not later overwrite the repaired header from a stale buffer. */
        if(fflush(file))fail(h,ESP_FAIL,"录音数据刷新失败");
        if(fsync(fileno(file)))fail(h,ESP_FAIL,"录音数据同步失败");
        if(fclose(file))fail(h,ESP_FAIL,"关闭录音文件失败");
        uint64_t saved=0;
        if(!repair_file(path,attempted,s->sample_rate,&saved))fail(h,ESP_FAIL,"WAV文件头修补失败，文件可能需要恢复");
        progress(s,saved);
    }
    if(leased)aether_storage_release(h->storage);
    if(s){free(s->ring);free(s->rx);free(s->output);free(s->stdio_buffer);free(s);}
    portENTER_CRITICAL(&h->lock);
    h->status.last_error=atomic_load(&h->error);h->status.peak=0;
    h->status.state=h->status.last_error==ESP_OK?AETHER_RECORDER_FINISHED:AETHER_RECORDER_ERROR;
    if(h->status.last_error==ESP_OK)text(h->status.detail,file?"录音已保存":"录音已取消",sizeof(h->status.detail));
    h->status.active=false; /* Final ownership publication; no h access below. */
    portEXIT_CRITICAL(&h->lock);vTaskDelete(NULL);
}

esp_err_t aether_recorder_new(ac101s_audio_handle_t audio,aether_storage_handle_t storage,aether_recorder_handle_t *out)
{
    if(!out||!audio||!storage)return ESP_ERR_INVALID_ARG;
    *out=NULL;
    aether_recorder_handle_t h=calloc(1,sizeof(*h));if(!h)return ESP_ERR_NO_MEM;
    h->audio=audio;h->storage=storage;h->lock=(portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    atomic_init(&h->stop,false);atomic_init(&h->pause,false);
    atomic_init(&h->screen_paused,false);atomic_init(&h->error,ESP_OK);
    h->commands=xSemaphoreCreateMutex();if(!h->commands){free(h);return ESP_ERR_NO_MEM;}
    aether_recorder_handle_t expected=NULL;
    if(!atomic_compare_exchange_strong(&s_default,&expected,h)){vSemaphoreDelete(h->commands);free(h);return ESP_ERR_INVALID_STATE;}
    *out=h;return ESP_OK;
}
aether_recorder_handle_t aether_recorder_get_default(void){return atomic_load(&s_default);}
esp_err_t aether_recorder_start(aether_recorder_handle_t h)
{
    return aether_recorder_start_mode(h, AETHER_RECORDER_ORIGINAL);
}
esp_err_t aether_recorder_start_mode(aether_recorder_handle_t h, aether_recorder_mode_t mode)
{
    if(!h || (mode != AETHER_RECORDER_ORIGINAL && mode != AETHER_RECORDER_VOICE_DENOISE))
        return ESP_ERR_INVALID_ARG;
    if(xSemaphoreTake(h->commands,0)!=pdTRUE)return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&h->lock);
    if(h->status.active||h->destroying){portEXIT_CRITICAL(&h->lock);xSemaphoreGive(h->commands);return ESP_ERR_INVALID_STATE;}
    h->requested_mode = mode;
    h->status.mode = AETHER_RECORDER_ORIGINAL;
    h->status.denoise_fallback = false;
    h->status.active=true;h->status.state=AETHER_RECORDER_STARTING;h->status.last_error=ESP_OK;
    h->status.data_bytes=h->status.duration_ms=h->status.peak=0;text(h->status.detail,"正在准备录音",sizeof(h->status.detail));
    atomic_store(&h->stop,false);atomic_store(&h->pause,false);atomic_store(&h->error,ESP_OK);
    portEXIT_CRITICAL(&h->lock);
    const bool created=create_task(writer_task,"rec_writer",h,3,
        mode==AETHER_RECORDER_VOICE_DENOISE && aether_recorder_denoise_supported())==pdPASS;
    if(!created){fail(h,ESP_ERR_NO_MEM,"无法创建录音写盘任务");portENTER_CRITICAL(&h->lock);h->status.state=AETHER_RECORDER_ERROR;h->status.active=false;portEXIT_CRITICAL(&h->lock);}
    xSemaphoreGive(h->commands);return created?ESP_OK:ESP_ERR_NO_MEM;
}
esp_err_t aether_recorder_set_paused(aether_recorder_handle_t h,bool paused)
{
    if(!h)return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    if(!h->status.active||atomic_load(&h->stop)){portEXIT_CRITICAL(&h->lock);return ESP_ERR_INVALID_STATE;}
    atomic_store(&h->pause,paused);portEXIT_CRITICAL(&h->lock);return ESP_OK;
}
void aether_recorder_set_screen_paused(aether_recorder_handle_t h, bool paused)
{
    if (h) atomic_store(&h->screen_paused, paused);
}
esp_err_t aether_recorder_stop(aether_recorder_handle_t h)
{
    if(!h)return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&h->lock);
    atomic_store(&h->stop,true);
    portEXIT_CRITICAL(&h->lock);
    return ESP_OK;
}
esp_err_t aether_recorder_get_status(aether_recorder_handle_t h,aether_recorder_status_t *status)
{if(!h||!status)return ESP_ERR_INVALID_ARG;portENTER_CRITICAL(&h->lock);*status=h->status;portEXIT_CRITICAL(&h->lock);return ESP_OK;}
esp_err_t aether_recorder_stop_wait(aether_recorder_handle_t h,uint32_t ms)
{
    if(!h)return ESP_ERR_INVALID_ARG;
    if(!ms){aether_recorder_stop(h);aether_recorder_status_t status;aether_recorder_get_status(h,&status);return status.active?ESP_ERR_TIMEOUT:ESP_OK;}
    const TickType_t began=xTaskGetTickCount(),budget=ms==UINT32_MAX?portMAX_DELAY:pdMS_TO_TICKS(ms);
    if(xSemaphoreTake(h->commands,budget)!=pdTRUE)return ESP_ERR_TIMEOUT;
    aether_recorder_stop(h);esp_err_t result=ESP_OK;
    for(;;){aether_recorder_status_t status;aether_recorder_get_status(h,&status);if(!status.active)break;
        if(budget!=portMAX_DELAY&&xTaskGetTickCount()-began>=budget){result=ESP_ERR_TIMEOUT;break;}vTaskDelay(1);}
    xSemaphoreGive(h->commands);return result;
}
esp_err_t aether_recorder_del(aether_recorder_handle_t h)
{
    if(!h)return ESP_ERR_INVALID_ARG;
    esp_err_t error=aether_recorder_stop_wait(h,5000);
    if(error!=ESP_OK)return error;
    if(xSemaphoreTake(h->commands,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&h->lock);h->destroying=true;portEXIT_CRITICAL(&h->lock);
    aether_recorder_handle_t expected=h;atomic_compare_exchange_strong(&s_default,&expected,NULL);
    xSemaphoreGive(h->commands);vSemaphoreDelete(h->commands);free(h);return ESP_OK;
}
