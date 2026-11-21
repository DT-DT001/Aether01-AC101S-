#include "aether_usb_audio.h"
#include "pcm_convert.h"
#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "usb/uac_host.h"

#define PCM_FIFO_SIZE 32768U
#define PCM_BLOCK_FRAMES 480U
#define PCM_WIRE_BYTES (PCM_BLOCK_FRAMES*4U)
#define PCM_NORMAL_BYTES 2304U
#define UAC_RING_BYTES 8192U
#define UAC_EVENT_COUNT 16U
#define CLEANUP_RETRY_MIN_MS 10U
#define CLEANUP_RETRY_MAX_MS 200U

typedef struct { uint8_t *data; size_t head, used; SemaphoreHandle_t lock; } pcm_fifo_t;
typedef struct { TickType_t due; unsigned command; uint16_t delay_ms; } cleanup_retry_t;
typedef struct {
    uac_host_device_handle_t device; /* Worker owns this and all UAC stream APIs. */
    bool started, supported, stream_resources;
    uint32_t rate;
    uint8_t channels;
    atomic_bool available, wanted, removed, transfer_error, cleanup;
    atomic_uint command;
    atomic_int fault_error;
    unsigned done_command;
    esp_err_t command_error;
    aether_pcm_converter_t converter;
    cleanup_retry_t retry; /* Worker-only; a new command bypasses old backoff. */
} audio_direction_t;
typedef struct { uint8_t address, interface; bool capture; } connect_event_t;
typedef struct {
    bool initialized, initializing;
    TaskHandle_t worker;
    QueueHandle_t events;
    SemaphoreHandle_t lifecycle;
    atomic_bool installed, stop_requested;
    audio_direction_t tx, rx;
    pcm_fifo_t tx_fifo, rx_fifo;
    int16_t *normal_tx, *wire_tx, *wire_rx, *normal_rx;
    size_t tx_pending;
    cleanup_retry_t class_retry;
    unsigned class_retry_tx, class_retry_rx;
    aether_usb_audio_status_t status;
} usb_audio_t;
static usb_audio_t s;
static portMUX_TYPE cache_lock=portMUX_INITIALIZER_UNLOCKED;

static void notify_worker(void) { if (s.worker) xTaskNotifyGive(s.worker); }
static TickType_t tick_ms(uint32_t ms) { TickType_t t=pdMS_TO_TICKS(ms); return ms && !t ? 1 : t; }
static void retry_reset(cleanup_retry_t *retry, unsigned command)
{
    *retry=(cleanup_retry_t){.command=command};
}
static bool retry_ready(cleanup_retry_t *retry, unsigned command)
{
    if(retry->command!=command)retry_reset(retry,command);
    return !retry->delay_ms || (int32_t)(xTaskGetTickCount()-retry->due)>=0;
}
static void retry_failed(cleanup_retry_t *retry)
{
    unsigned delay=retry->delay_ms ? (unsigned)retry->delay_ms*2U : CLEANUP_RETRY_MIN_MS;
    retry->delay_ms=(uint16_t)(delay>CLEANUP_RETRY_MAX_MS ? CLEANUP_RETRY_MAX_MS : delay);
    retry->due=xTaskGetTickCount()+tick_ms(retry->delay_ms);
}
static TickType_t retry_wait(const cleanup_retry_t *retry, unsigned command)
{
    if(retry->command!=command || !retry->delay_ms)return 0;
    const int32_t left=(int32_t)(retry->due-xTaskGetTickCount());
    return left>0 ? (TickType_t)left : 0;
}

static void set_error(esp_err_t e)
{
    portENTER_CRITICAL(&cache_lock); s.status.last_error=e; portEXIT_CRITICAL(&cache_lock);
}
static void fifo_levels(void)
{
    size_t tx=0,rx=0;bool have_tx=false,have_rx=false;
    if(xSemaphoreTake(s.tx_fifo.lock,0)==pdTRUE){tx=s.tx_fifo.used;have_tx=true;xSemaphoreGive(s.tx_fifo.lock);}
    if(xSemaphoreTake(s.rx_fifo.lock,0)==pdTRUE){rx=s.rx_fifo.used;have_rx=true;xSemaphoreGive(s.rx_fifo.lock);}
    portENTER_CRITICAL(&cache_lock);
    if(have_tx)s.status.tx_fifo_bytes=tx;
    if(have_rx)s.status.rx_fifo_bytes=rx;
    portEXIT_CRITICAL(&cache_lock);
}
static bool fifo_reset(pcm_fifo_t *f)
{
    if(xSemaphoreTake(f->lock,tick_ms(10))!=pdTRUE)return false;
    f->head=f->used=0;xSemaphoreGive(f->lock);return true;
}
static bool fifo_put(pcm_fifo_t *f,const void *data,size_t bytes)
{
    if(xSemaphoreTake(f->lock,tick_ms(1))!=pdTRUE)return false;
    bool ok=bytes<=PCM_FIFO_SIZE-f->used;
    if(ok){size_t tail=(f->head+f->used)%PCM_FIFO_SIZE,n=PCM_FIFO_SIZE-tail;
        if(n>bytes){n=bytes;}
        memcpy(f->data+tail,data,n);memcpy(f->data,(const uint8_t*)data+n,bytes-n);f->used+=bytes;}
    xSemaphoreGive(f->lock);return ok;
}
static size_t fifo_take(pcm_fifo_t *f,void *data,size_t bytes,bool whole)
{
    if(xSemaphoreTake(f->lock,tick_ms(1))!=pdTRUE)return 0;
    if(whole&&f->used<bytes){xSemaphoreGive(f->lock);return 0;}
    if(bytes>f->used)bytes=f->used;
    bytes&=~(size_t)3; /* normalized stereo-frame boundary */
    size_t n=PCM_FIFO_SIZE-f->head;if(n>bytes)n=bytes;
    memcpy(data,f->data+f->head,n);memcpy((uint8_t*)data+n,f->data,bytes-n);
    f->head=(f->head+bytes)%PCM_FIFO_SIZE;f->used-=bytes;
    xSemaphoreGive(f->lock);return bytes;
}
static void publish_direction(audio_direction_t *d,esp_err_t error,unsigned command)
{
    const bool tx=d==&s.tx;
    portENTER_CRITICAL(&cache_lock);
    d->done_command=command;d->command_error=error;
    if(tx){s.status.playback_active=d->started;s.status.playback_ready=d->device&&d->supported&&!atomic_load(&d->removed);
        s.status.playback_error=error;}
    else{s.status.capture_active=d->started;s.status.capture_ready=d->device&&d->supported&&!atomic_load(&d->removed);
        s.status.capture_error=error;}
    s.status.cleanup_pending=atomic_load(&s.stop_requested)&&atomic_load(&s.installed);
    s.status.cleanup_pending|=atomic_load(&s.tx.cleanup)||atomic_load(&s.rx.cleanup);
    if(error!=ESP_OK)s.status.last_error=error;
    portEXIT_CRITICAL(&cache_lock);
}
static bool direction_allowed(const audio_direction_t *d)
{
    return atomic_load(&s.installed)&&!atomic_load(&s.stop_requested)&&atomic_load(&d->available)&&
        !atomic_load(&d->removed)&&!atomic_load(&d->transfer_error)&&!atomic_load(&d->cleanup);
}
static void device_event(uac_host_device_handle_t device,uac_host_device_event_t event,void *arg)
{
    (void)device;audio_direction_t *d=arg;
    if(event==UAC_HOST_DRIVER_EVENT_DISCONNECTED){
        atomic_store(&d->removed,true);atomic_store(&d->available,false);atomic_store(&d->wanted,false);
        atomic_fetch_add(&d->command,1);
        portENTER_CRITICAL(&cache_lock);++s.status.disconnects;++s.status.generation;portEXIT_CRITICAL(&cache_lock);
    }else if(event==UAC_HOST_DEVICE_EVENT_TX_DONE){
        portENTER_CRITICAL(&cache_lock);++s.status.tx_low_watermarks;portEXIT_CRITICAL(&cache_lock);
    }else if(event==UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR){
        atomic_store(&d->fault_error,ESP_FAIL);atomic_store(&d->transfer_error,true);atomic_store(&d->available,false);atomic_store(&d->wanted,false);
        atomic_fetch_add(&d->command,1);
        portENTER_CRITICAL(&cache_lock);++s.status.transfer_errors;s.status.last_error=ESP_FAIL;portEXIT_CRITICAL(&cache_lock);
    }
    notify_worker(); /* Never acquire a codec, FIFO or UAC lock in a callback. */
}
static void driver_event(uint8_t addr,uint8_t iface,uac_host_driver_event_t event,void *arg)
{
    (void)arg;if(atomic_load(&s.stop_requested))return;
    connect_event_t e={.address=addr,.interface=iface,.capture=event==UAC_HOST_DRIVER_EVENT_RX_CONNECTED};
    if(xQueueSend(s.events,&e,0)!=pdTRUE){set_error(ESP_ERR_NO_MEM);atomic_store(&s.stop_requested,true);}
    notify_worker();
}
static bool frequency_supported(const uac_host_dev_alt_param_t *p,uint32_t rate)
{
    if(!p->sample_freq_type)return rate>=p->sample_freq_lower&&rate<=p->sample_freq_upper;
    unsigned n=p->sample_freq_type;if(n>UAC_FREQ_NUM_MAX)n=UAC_FREQ_NUM_MAX;
    for(unsigned i=0;i<n;++i)if(p->sample_freq[i]==rate)return true;
    return false;
}
static esp_err_t validate_wire_layout(audio_direction_t *d,const uac_host_dev_info_t *info,uint32_t rate,uint8_t channels)
{
    /* Official1.5 chooses a matching channels/bits/rate tuple without checking
     * format or subframe size. Every alt it may select must be packed PCM16. */
    for(unsigned alt=1;alt<=info->iface_alt_num;++alt){
        if(atomic_load(&s.stop_requested)||atomic_load(&d->removed)||atomic_load(&d->transfer_error))return ESP_ERR_INVALID_STATE;
        uac_host_dev_alt_param_t p={0};
        const esp_err_t e=uac_host_get_device_alt_param(d->device,(uint8_t)alt,&p);
        if(e!=ESP_OK)return e;
        if(p.channels==channels&&p.bit_resolution==16&&frequency_supported(&p,rate)&&
            (p.format!=1||p.subframe_size!=2))return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}
static esp_err_t select_format(audio_direction_t *d,const uac_host_dev_info_t *info)
{
    static const uint32_t rates[]={48000,44100};
    /* Official1.5 retains bSamFreqType but truncates the stored frequency array;
     * its start path iterates bSamFreqType. Reject such interfaces before that
     * upstream path can read beyond UAC_FREQ_NUM_MAX, including later alts. */
    for(unsigned alt=1;alt<=info->iface_alt_num;++alt){
        if(atomic_load(&s.stop_requested)||atomic_load(&d->removed)||atomic_load(&d->transfer_error))return ESP_ERR_INVALID_STATE;
        uac_host_dev_alt_param_t p={0};
        esp_err_t e=uac_host_get_device_alt_param(d->device,(uint8_t)alt,&p);
        if(e!=ESP_OK)return e;
        if(p.sample_freq_type>UAC_FREQ_NUM_MAX)return ESP_ERR_NOT_SUPPORTED;
    }
    for(unsigned rate=0;rate<2;++rate)for(unsigned preference=0;preference<2;++preference)
        for(unsigned alt=1;alt<=info->iface_alt_num;++alt){
            uac_host_dev_alt_param_t p={0};
            esp_err_t e=uac_host_get_device_alt_param(d->device,(uint8_t)alt,&p);
            if(e!=ESP_OK)return e;
            const uint8_t channels=(uint8_t)(preference?1:2);
            if(p.format==1&&p.channels==channels&&p.bit_resolution==16&&p.subframe_size==2&&frequency_supported(&p,rates[rate])){
                const esp_err_t layout=validate_wire_layout(d,info,rates[rate],channels);
                if(layout!=ESP_OK)return layout;
                d->rate=rates[rate];d->channels=channels;return ESP_OK;
            }
        }
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t stop_direction(audio_direction_t *d)
{
    if(d->device&&d->stream_resources){
        esp_err_t e=uac_host_device_stop(d->device);
        if(e!=ESP_OK){atomic_store(&d->cleanup,true);return e;} /* Retain unknown DMA lifetime. */
        d->started=false;d->stream_resources=false;
    }
    if(!fifo_reset(d==&s.tx?&s.tx_fifo:&s.rx_fifo)){atomic_store(&d->cleanup,true);return ESP_ERR_TIMEOUT;}
    if(d==&s.tx)s.tx_pending=0;
    atomic_store(&d->cleanup,false);return ESP_OK;
}
static esp_err_t close_direction(audio_direction_t *d)
{
    esp_err_t e=stop_direction(d);if(e!=ESP_OK)return e;
    if(d->device){e=uac_host_device_close(d->device);if(e!=ESP_OK){atomic_store(&d->cleanup,true);return e;}d->device=NULL;}
    d->supported=false;d->rate=0;d->channels=0;atomic_store(&d->available,false);atomic_store(&d->cleanup,false);
    portENTER_CRITICAL(&cache_lock);
    if(d==&s.tx){s.status.playback_connected=false;s.status.playback_ready=false;s.status.playback_active=false;s.status.hardware_volume_supported=false;}
    else{s.status.capture_connected=false;s.status.capture_ready=false;s.status.capture_active=false;}
    portEXIT_CRITICAL(&cache_lock);
    return ESP_OK;
}
static bool opening_cancelled(audio_direction_t *d,unsigned command)
{
    return atomic_load(&s.stop_requested)||atomic_load(&d->removed)||
        atomic_load(&d->transfer_error)||atomic_load(&d->command)!=command;
}
static void open_direction(const connect_event_t *event)
{
    audio_direction_t *d=event->capture?&s.rx:&s.tx;
    if(d->device||atomic_load(&s.stop_requested))return; /* First supported endpoint per direction. */
    atomic_store(&d->available,false);
    atomic_store(&d->removed,false);atomic_store(&d->transfer_error,false);atomic_store(&d->fault_error,ESP_OK);
    const unsigned opening_command=atomic_load(&d->command);
    uac_host_device_config_t cfg={.addr=event->address,.iface_num=event->interface,
        .buffer_size=UAC_RING_BYTES,.buffer_threshold=1024,.callback=device_event,.callback_arg=d};
    esp_err_t e=uac_host_device_open(&cfg,&d->device);
    if(e==ESP_OK){
        uac_host_dev_info_t info={0};
        if(opening_cancelled(d,opening_command))e=ESP_ERR_INVALID_STATE;
        if(e==ESP_OK)e=uac_host_get_device_info(d->device,&info);
        if(e==ESP_OK&&opening_cancelled(d,opening_command))e=ESP_ERR_INVALID_STATE;
        if(e==ESP_OK)e=select_format(d,&info);
        if(e==ESP_OK&&opening_cancelled(d,opening_command))e=ESP_ERR_INVALID_STATE;
        portENTER_CRITICAL(&cache_lock);
        ++s.status.generation;
        if(event->capture){s.status.capture_connected=e==ESP_OK;s.status.capture_rate_hz=d->rate;s.status.capture_channels=d->channels;
            s.status.capture_vid=info.VID;s.status.capture_pid=info.PID;}
        else{s.status.playback_connected=e==ESP_OK;s.status.playback_rate_hz=d->rate;s.status.playback_channels=d->channels;
            s.status.playback_vid=info.VID;s.status.playback_pid=info.PID;s.status.hardware_volume_supported=false;}
        portEXIT_CRITICAL(&cache_lock);
    }
    d->supported=e==ESP_OK;
    /* Never erase a disconnect/error published by the background class task
     * during open or descriptor negotiation. Its flag keeps availability gated. */
    atomic_store(&d->available,d->supported&&!opening_cancelled(d,opening_command));
    if(e!=ESP_OK){
        portENTER_CRITICAL(&cache_lock);if(e==ESP_ERR_NOT_SUPPORTED)++s.status.unsupported_interfaces;portEXIT_CRITICAL(&cache_lock);
        /* Retain failed-close handle; the Host owner cannot be released early. */
        if(d->device){esp_err_t ce=uac_host_device_close(d->device);if(ce==ESP_OK)d->device=NULL;else{e=ce;atomic_store(&d->cleanup,true);atomic_store(&d->transfer_error,true);}}
    }
    publish_direction(d,e,atomic_load(&d->command));
}
static esp_err_t start_direction(audio_direction_t *d)
{
    if(!direction_allowed(d)||!d->device||!d->supported)return ESP_ERR_INVALID_STATE;
    if(d->started)return ESP_OK;
    if(!fifo_reset(d==&s.tx?&s.tx_fifo:&s.rx_fifo))return ESP_ERR_TIMEOUT;
    if(d==&s.tx){s.tx_pending=0;aether_pcm_converter_init(&d->converter,48000,2,d->rate,d->channels);}
    else aether_pcm_converter_init(&d->converter,d->rate,d->channels,48000,2);
    const uac_host_stream_config_t cfg={.channels=d->channels,.bit_resolution=16,.sample_freq=d->rate,.flags=0};
    d->stream_resources=true; /* Even failed start can need a checked stop. */
    esp_err_t e=uac_host_device_start(d->device,&cfg);
    if(e!=ESP_OK)return e;
    d->started=true;
    /* Existing board audio route already applies the single logical stream/master
     * software gain. Hardware features are normalized once, never multiplied. */
    e=uac_host_device_set_mute(d->device,false);
    if(e==ESP_ERR_NOT_SUPPORTED)e=ESP_OK;
    if(e==ESP_OK&&d==&s.tx){
        const esp_err_t volume=uac_host_device_set_volume_db(d->device,0.0f);
        portENTER_CRITICAL(&cache_lock);s.status.hardware_volume_supported=volume==ESP_OK;portEXIT_CRITICAL(&cache_lock);
        if(volume!=ESP_OK&&volume!=ESP_ERR_NOT_SUPPORTED&&volume!=ESP_ERR_INVALID_ARG)e=volume;
    }
    return e;
}
static void service_direction(audio_direction_t *d)
{
    unsigned command; bool wanted;
    portENTER_CRITICAL(&cache_lock);command=atomic_load(&d->command);wanted=atomic_load(&d->wanted);portEXIT_CRITICAL(&cache_lock);
    const bool fault=atomic_load(&d->removed)||atomic_load(&d->transfer_error);
    if(fault||atomic_load(&d->cleanup)){
        if(!retry_ready(&d->retry,command))return;
        const esp_err_t cause=atomic_load(&d->removed)?ESP_ERR_INVALID_STATE:atomic_load(&d->fault_error);
        /* Failed ordinary STOP/start rollback also needs a retry, but must
         * never implicitly resume the failed stream after cleanup succeeds. */
        esp_err_t e=fault?close_direction(d):stop_direction(d);
        if(e==ESP_OK){
            retry_reset(&d->retry,command);
            if(fault){atomic_store(&d->removed,false);atomic_store(&d->transfer_error,false);e=cause==ESP_OK?ESP_FAIL:cause;}
        }else retry_failed(&d->retry);
        publish_direction(d,e,command);return;
    }
    const bool desired=wanted&&!atomic_load(&s.stop_requested);
    if(command==d->done_command)return;
    retry_reset(&d->retry,command);
    esp_err_t e=desired?start_direction(d):stop_direction(d);
    if(desired&&(command!=atomic_load(&d->command)||!atomic_load(&d->wanted)||!direction_allowed(d))){
        esp_err_t off=stop_direction(d);e=off==ESP_OK?ESP_ERR_INVALID_STATE:off;
    }
    if(e!=ESP_OK&&desired){ /* A failed start must not leave a running hidden stream. */
        esp_err_t off=stop_direction(d);if(off!=ESP_OK)e=off;
    }
    if(atomic_load(&d->cleanup))retry_failed(&d->retry);
    publish_direction(d,e,command);
}
static esp_err_t close_for_class_stop(audio_direction_t *d)
{
    const unsigned command=atomic_load(&d->command);
    if(!retry_ready(&d->retry,command))return ESP_ERR_NOT_FINISHED;
    const esp_err_t e=close_direction(d);
    if(e==ESP_OK){
        retry_reset(&d->retry,command);
        atomic_store(&d->removed,false);atomic_store(&d->transfer_error,false);
    }else retry_failed(&d->retry);
    publish_direction(d,e,command);
    return e;
}
static void pump_playback(void)
{
    audio_direction_t *d=&s.tx;if(!d->started||!atomic_load(&d->wanted)||!direction_allowed(d))return;
    if(!s.tx_pending){
        const size_t bytes=fifo_take(&s.tx_fifo,s.normal_tx,PCM_WIRE_BYTES,false);
        if(!bytes)return;
        size_t consumed=0;const size_t frames=aether_pcm_convert(&d->converter,s.normal_tx,bytes/4,s.wire_tx,
            PCM_BLOCK_FRAMES,&consumed);
        if(consumed!=bytes/4){atomic_store(&d->fault_error,ESP_ERR_INVALID_SIZE);atomic_store(&d->transfer_error,true);set_error(ESP_ERR_INVALID_SIZE);return;}
        s.tx_pending=frames*d->channels*2U;
    }
    if(!s.tx_pending)return;
    const esp_err_t e=uac_host_device_write(d->device,(uint8_t*)s.wire_tx,(uint32_t)s.tx_pending,0);
    if(e==ESP_OK){portENTER_CRITICAL(&cache_lock);s.status.tx_submitted_bytes+=s.tx_pending;portEXIT_CRITICAL(&cache_lock);s.tx_pending=0;}
    else if(e!=ESP_FAIL){atomic_store(&d->fault_error,e);atomic_store(&d->transfer_error,true);atomic_store(&d->wanted,false);set_error(e);}
}
static void pump_capture(void)
{
    audio_direction_t *d=&s.rx;if(!d->started||!atomic_load(&d->wanted)||!direction_allowed(d))return;
    uint32_t bytes=0;const uint32_t request=(d->rate/100U)*d->channels*2U;
    const esp_err_t e=uac_host_device_read(d->device,(uint8_t*)s.wire_rx,request,&bytes,0);
    if(e==ESP_ERR_TIMEOUT)return;
    if(e!=ESP_OK||bytes%(d->channels*2U)){atomic_store(&d->fault_error,e==ESP_OK?ESP_ERR_INVALID_SIZE:e);atomic_store(&d->transfer_error,true);atomic_store(&d->wanted,false);set_error(e==ESP_OK?ESP_ERR_INVALID_SIZE:e);return;}
    if(!bytes)return;
    size_t consumed=0;const size_t frames=aether_pcm_convert(&d->converter,s.wire_rx,bytes/(d->channels*2U),
        s.normal_rx,PCM_NORMAL_BYTES/4U,&consumed);
    if(consumed!=bytes/(d->channels*2U)||!fifo_put(&s.rx_fifo,s.normal_rx,frames*4U)){
        portENTER_CRITICAL(&cache_lock);++s.status.rx_overruns;portEXIT_CRITICAL(&cache_lock);
        atomic_store(&d->fault_error,ESP_ERR_NO_MEM);atomic_store(&d->transfer_error,true);atomic_store(&d->wanted,false);set_error(ESP_ERR_NO_MEM);return;
    }
    portENTER_CRITICAL(&cache_lock);s.status.rx_received_bytes+=frames*4U;portEXIT_CRITICAL(&cache_lock);
}
static void service_once(void)
{
    if(atomic_load(&s.installed)){
        connect_event_t event;
        while(xQueueReceive(s.events,&event,0)==pdTRUE)if(!atomic_load(&s.stop_requested))open_direction(&event);
        if(atomic_load(&s.stop_requested)){
            const esp_err_t tx=close_for_class_stop(&s.tx),rx=close_for_class_stop(&s.rx);
            if(tx==ESP_OK&&rx==ESP_OK){
                const unsigned tx_command=atomic_load(&s.tx.command),rx_command=atomic_load(&s.rx.command);
                if(s.class_retry_tx!=tx_command || s.class_retry_rx!=rx_command){
                    retry_reset(&s.class_retry,0);s.class_retry_tx=tx_command;s.class_retry_rx=rx_command;
                }
                if(retry_ready(&s.class_retry,0)){
                    const esp_err_t e=uac_host_uninstall();
                    if(e==ESP_OK){
                        retry_reset(&s.class_retry,0);atomic_store(&s.installed,false);
                        portENTER_CRITICAL(&cache_lock);s.status.installed=false;s.status.stopping=false;s.status.cleanup_pending=false;portEXIT_CRITICAL(&cache_lock);
                    }else{retry_failed(&s.class_retry);set_error(e);}
                }
            }else{
                const esp_err_t e=tx!=ESP_OK&&tx!=ESP_ERR_NOT_FINISHED?tx:
                    rx!=ESP_OK&&rx!=ESP_ERR_NOT_FINISHED?rx:ESP_OK;
                if(e!=ESP_OK)set_error(e);
            }
        }else{
            service_direction(&s.tx);service_direction(&s.rx);
            pump_playback();pump_capture();
        }
        fifo_levels();
    }
}
/* No polling is needed for an installed HID-only/idle UAC client. Connect,
 * commands, PCM writes and physical disconnects retain a task notification.
 * Active audio keeps its original2ms service cadence; cleanup failures retain
 * ownership and retry on a bounded timer, not a high-priority error spin. */
static TickType_t worker_wait_ticks(void)
{
    if(!atomic_load(&s.installed))return portMAX_DELAY;
    TickType_t wait=portMAX_DELAY;
    audio_direction_t *directions[]={&s.tx,&s.rx};
    for(unsigned i=0;i<2;++i){
        const audio_direction_t *d=directions[i];
        const unsigned command=atomic_load(&d->command);
        const bool cleaning=atomic_load(&d->removed)||atomic_load(&d->transfer_error)||atomic_load(&d->cleanup);
        TickType_t due=portMAX_DELAY;
        if(cleaning)due=retry_wait(&d->retry,command);
        else if(command!=d->done_command)due=0;
        else if(d->started&&atomic_load(&d->wanted)&&direction_allowed(d))due=tick_ms(2);
        if(due<wait)wait=due;
    }
    if(atomic_load(&s.stop_requested)){
        const unsigned tx_command=atomic_load(&s.tx.command),rx_command=atomic_load(&s.rx.command);
        /* Once both endpoints are closed, only driver uninstall can remain. */
        if(!s.tx.device&&!s.rx.device&&!atomic_load(&s.tx.cleanup)&&!atomic_load(&s.rx.cleanup)){
            const TickType_t due=s.class_retry_tx!=tx_command || s.class_retry_rx!=rx_command ?
                0:retry_wait(&s.class_retry,0);
            if(due<wait)wait=due;
        }
    }
    return wait;
}
static void worker(void *arg)
{
    (void)arg;
    for(;;){service_once();ulTaskNotifyTake(pdTRUE,worker_wait_ticks());}
}
static void free_initial_resources(void)
{
    if(s.tx_fifo.lock)vSemaphoreDelete(s.tx_fifo.lock);
    if(s.rx_fifo.lock)vSemaphoreDelete(s.rx_fifo.lock);
    if(s.lifecycle)vSemaphoreDelete(s.lifecycle);
    if(s.events)vQueueDelete(s.events);
    heap_caps_free(s.tx_fifo.data);heap_caps_free(s.rx_fifo.data);
    heap_caps_free(s.normal_tx);heap_caps_free(s.wire_tx);heap_caps_free(s.wire_rx);heap_caps_free(s.normal_rx);
    s.tx_fifo.lock=s.rx_fifo.lock=s.lifecycle=NULL;s.events=NULL;
    s.tx_fifo.data=s.rx_fifo.data=NULL;s.normal_tx=s.wire_tx=s.wire_rx=s.normal_rx=NULL;
}
esp_err_t aether_usb_audio_init(void)
{
    portENTER_CRITICAL(&cache_lock);
    if(s.initialized){portEXIT_CRITICAL(&cache_lock);return ESP_OK;}
    if(s.initializing){portEXIT_CRITICAL(&cache_lock);return ESP_ERR_NOT_FINISHED;}
    s.initializing=true;portEXIT_CRITICAL(&cache_lock);
    s.tx_fifo.lock=xSemaphoreCreateMutex();s.rx_fifo.lock=xSemaphoreCreateMutex();s.lifecycle=xSemaphoreCreateMutex();
    s.events=xQueueCreate(UAC_EVENT_COUNT,sizeof(connect_event_t));
    s.tx_fifo.data=heap_caps_malloc(PCM_FIFO_SIZE,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s.rx_fifo.data=heap_caps_malloc(PCM_FIFO_SIZE,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s.normal_tx=heap_caps_malloc(PCM_WIRE_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s.wire_tx=heap_caps_malloc(PCM_WIRE_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s.wire_rx=heap_caps_malloc(PCM_WIRE_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s.normal_rx=heap_caps_malloc(PCM_NORMAL_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    bool ok=s.tx_fifo.lock&&s.rx_fifo.lock&&s.lifecycle&&s.events&&s.tx_fifo.data&&s.rx_fifo.data&&s.normal_tx&&s.wire_tx&&s.wire_rx&&s.normal_rx;
    if(ok){atomic_init(&s.installed,false);atomic_init(&s.stop_requested,true);
        atomic_init(&s.tx.available,false);atomic_init(&s.tx.wanted,false);atomic_init(&s.tx.removed,false);atomic_init(&s.tx.transfer_error,false);atomic_init(&s.tx.command,0);atomic_init(&s.tx.cleanup,false);atomic_init(&s.tx.fault_error,ESP_OK);
        atomic_init(&s.rx.available,false);atomic_init(&s.rx.wanted,false);atomic_init(&s.rx.removed,false);atomic_init(&s.rx.transfer_error,false);atomic_init(&s.rx.command,0);atomic_init(&s.rx.cleanup,false);atomic_init(&s.rx.fault_error,ESP_OK);
        ok=xTaskCreate(worker,"usb_audio",8192,NULL,6,&s.worker)==pdPASS;}
    if(!ok)free_initial_resources();
    portENTER_CRITICAL(&cache_lock);s.initializing=false;s.initialized=ok;portEXIT_CRITICAL(&cache_lock);
    return ok?ESP_OK:ESP_ERR_NO_MEM;
}
esp_err_t aether_usb_audio_class_start(void *context)
{
    (void)context;esp_err_t e=aether_usb_audio_init();if(e!=ESP_OK)return e;
    if(xSemaphoreTake(s.lifecycle,tick_ms(100))!=pdTRUE)return ESP_ERR_TIMEOUT;
    if(atomic_load(&s.installed)){xSemaphoreGive(s.lifecycle);return ESP_ERR_INVALID_STATE;}
    atomic_store(&s.stop_requested,false);
    retry_reset(&s.tx.retry,atomic_load(&s.tx.command));retry_reset(&s.rx.retry,atomic_load(&s.rx.command));
    retry_reset(&s.class_retry,0);
    s.class_retry_tx=atomic_load(&s.tx.command);s.class_retry_rx=atomic_load(&s.rx.command);
    const uac_host_driver_config_t cfg={.create_background_task=true,.task_priority=6,.stack_size=6144,
        .core_id=tskNO_AFFINITY,.callback=driver_event,.callback_arg=NULL};
    e=uac_host_install(&cfg);
    if(e==ESP_OK){atomic_store(&s.installed,true);portENTER_CRITICAL(&cache_lock);
        s.status.installed=true;s.status.stopping=false;s.status.cleanup_pending=false;++s.status.generation;s.status.last_error=ESP_OK;portEXIT_CRITICAL(&cache_lock);notify_worker();}
    else{atomic_store(&s.stop_requested,true);set_error(e);}
    xSemaphoreGive(s.lifecycle);return e;
}
void aether_usb_audio_class_request_stop(void *context)
{
    (void)context;if(!s.initialized)return;
    atomic_store(&s.stop_requested,true);atomic_store(&s.tx.wanted,false);atomic_store(&s.rx.wanted,false);
    atomic_store(&s.tx.available,false);atomic_store(&s.rx.available,false);
    atomic_fetch_add(&s.tx.command,1);atomic_fetch_add(&s.rx.command,1);
    portENTER_CRITICAL(&cache_lock);s.status.stopping=atomic_load(&s.installed);s.status.cleanup_pending=s.status.stopping;portEXIT_CRITICAL(&cache_lock);
    notify_worker();
}
esp_err_t aether_usb_audio_class_poll_stopped(void *context)
{
    (void)context;return !s.initialized||!atomic_load(&s.installed)?ESP_OK:ESP_ERR_NOT_FINISHED;
}
esp_err_t aether_usb_audio_get_status(aether_usb_audio_status_t *status)
{
    if(!status)return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&cache_lock);*status=s.status;portEXIT_CRITICAL(&cache_lock);return ESP_OK;
}
bool aether_usb_audio_playback_connected(void){return s.initialized&&direction_allowed(&s.tx);}
bool aether_usb_audio_capture_connected(void){return s.initialized&&direction_allowed(&s.rx);}
static esp_err_t stream_command(audio_direction_t *d,bool start,uint32_t timeout_ms)
{
    if(!s.initialized)return start?ESP_ERR_INVALID_STATE:ESP_OK;
    if(!start&&!atomic_load(&s.installed))return ESP_OK;
    portENTER_CRITICAL(&cache_lock);
    if(start&&!direction_allowed(d)){portEXIT_CRITICAL(&cache_lock);return ESP_ERR_INVALID_STATE;}
    unsigned command=atomic_load(&d->command);
    /* Concurrent STOP callers join one in-flight stop instead of cancelling
     * each other's acknowledgment. A completed failed stop publishes a retry. */
    if(start||atomic_load(&d->wanted)||d->done_command==command){
        atomic_store(&d->wanted,start);command=atomic_fetch_add(&d->command,1)+1;
    }
    portEXIT_CRITICAL(&cache_lock);notify_worker();
    const TickType_t begin=xTaskGetTickCount(),budget=tick_ms(timeout_ms);
    for(;;){
        unsigned done;esp_err_t error;bool active;
        portENTER_CRITICAL(&cache_lock);
        done=d->done_command;error=d->command_error;
        active=d==&s.tx?s.status.playback_active:s.status.capture_active;
        portEXIT_CRITICAL(&cache_lock);
        const unsigned latest=atomic_load(&d->command);
        if(latest!=command){
            if(!start&&!atomic_load(&d->wanted))command=latest; /* Join disconnect/OFF cleanup. */
            else return ESP_ERR_INVALID_STATE;
        }
        if(done==command){
            if(start&&error==ESP_OK&&(!active||!atomic_load(&d->wanted)||!direction_allowed(d)))
                return ESP_ERR_INVALID_STATE; /* A STOP ACK cannot satisfy START. */
            if(!start&&!active&&!atomic_load(&d->cleanup))return ESP_OK;
            return error;
        }
        if(!start&&!atomic_load(&s.installed))return ESP_OK;
        if(xTaskGetTickCount()-begin>=budget){
            if(start){portENTER_CRITICAL(&cache_lock);if(atomic_load(&d->command)==command){atomic_store(&d->wanted,false);atomic_fetch_add(&d->command,1);}portEXIT_CRITICAL(&cache_lock);notify_worker();}
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(tick_ms(1));
    }
}
esp_err_t aether_usb_audio_playback_start(uint32_t t){return stream_command(&s.tx,true,t);}
esp_err_t aether_usb_audio_playback_stop(uint32_t t){return stream_command(&s.tx,false,t);}
esp_err_t aether_usb_audio_capture_start(uint32_t t){return stream_command(&s.rx,true,t);}
esp_err_t aether_usb_audio_capture_stop(uint32_t t){return stream_command(&s.rx,false,t);}
static esp_err_t pcm_io(audio_direction_t *d,void *data,size_t bytes,uint32_t timeout_ms,bool writing)
{
    if(!data||!bytes||bytes%4||bytes>PCM_FIFO_SIZE)return ESP_ERR_INVALID_ARG;
    if(!s.initialized)return ESP_ERR_INVALID_STATE;
    const unsigned command=atomic_load(&d->command);
    const TickType_t begin=xTaskGetTickCount(),budget=tick_ms(timeout_ms);
    for(;;){
        if(!direction_allowed(d)||!atomic_load(&d->wanted)||atomic_load(&d->command)!=command)return ESP_ERR_INVALID_STATE;
        bool active;portENTER_CRITICAL(&cache_lock);active=d==&s.tx?s.status.playback_active:s.status.capture_active;portEXIT_CRITICAL(&cache_lock);
        if(!active)return ESP_ERR_INVALID_STATE;
        const bool ok=writing?fifo_put(&s.tx_fifo,data,bytes):fifo_take(&s.rx_fifo,data,bytes,true)==bytes;
        if(ok){
            if(!direction_allowed(d)||!atomic_load(&d->wanted)||atomic_load(&d->command)!=command)return ESP_ERR_INVALID_STATE;
            if(writing){portENTER_CRITICAL(&cache_lock);s.status.tx_queued_bytes+=bytes;portEXIT_CRITICAL(&cache_lock);notify_worker();}
            return ESP_OK;
        }
        if(xTaskGetTickCount()-begin>=budget){if(writing){portENTER_CRITICAL(&cache_lock);++s.status.tx_timeouts;portEXIT_CRITICAL(&cache_lock);}return ESP_ERR_TIMEOUT;}
        vTaskDelay(tick_ms(1));
    }
}
esp_err_t aether_usb_audio_write(const void *pcm,size_t bytes,uint32_t timeout_ms){return pcm_io(&s.tx,(void*)pcm,bytes,timeout_ms,true);}
esp_err_t aether_usb_audio_capture_read(void *pcm,size_t bytes,uint32_t timeout_ms){return pcm_io(&s.rx,pcm,bytes,timeout_ms,false);}
