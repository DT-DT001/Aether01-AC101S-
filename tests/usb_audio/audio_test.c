#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../components/aether_usb_audio/aether_usb_audio.c"
unsigned critical_depth;
struct fake_mutex{bool locked;};
struct fake_queue{unsigned size,capacity,head,used;uint8_t *data;};
struct fake_uac{bool opened,started;uint32_t rate;uint8_t channels;uac_host_device_event_cb_t callback;void *arg;uint8_t rx[4096];size_t rx_bytes;uint8_t tx[8192];size_t tx_bytes;};
static struct fake_uac tx,rx;
static uac_host_driver_event_cb_t connected_cb;
static void *connected_arg;
static TickType_t ticks;
static bool auto_service=true, cancel_start, bad_format, rate44100, mono_device, continuous_format;
static esp_err_t start_error,stop_error,close_error,write_error,uninstall_error;
static unsigned install_calls,uninstall_calls,open_calls,start_calls,stop_calls,close_calls,notification_calls;
static struct fake_uac *stop_error_device;
static unsigned disconnect_open_phase;
static bool excessive_frequencies, maximum_frequencies, nested_stop, off_before_service, ambiguous_layout, ambiguous_codec;
SemaphoreHandle_t xSemaphoreCreateMutex(void){return calloc(1,sizeof(struct fake_mutex));}
BaseType_t xSemaphoreTake(SemaphoreHandle_t m,TickType_t t){(void)t;assert(m);if(m->locked)return pdFALSE;m->locked=true;return pdTRUE;}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m){assert(m&&m->locked);m->locked=false;return pdTRUE;}
void vSemaphoreDelete(SemaphoreHandle_t m){assert(m&&!m->locked);free(m);}
QueueHandle_t xQueueCreate(UBaseType_t capacity,UBaseType_t size){QueueHandle_t q=calloc(1,sizeof(*q));q->capacity=capacity;q->size=size;q->data=calloc(capacity,size);return q;}
BaseType_t xQueueSend(QueueHandle_t q,const void*p,TickType_t t){(void)t;if(q->used==q->capacity)return pdFALSE;memcpy(q->data+((q->head+q->used)%q->capacity)*q->size,p,q->size);++q->used;return pdTRUE;}
BaseType_t xQueueReceive(QueueHandle_t q,void*p,TickType_t t){(void)t;if(!q->used)return pdFALSE;memcpy(p,q->data+q->head*q->size,q->size);q->head=(q->head+1)%q->capacity;--q->used;return pdTRUE;}
void vQueueDelete(QueueHandle_t q){free(q->data);free(q);}
BaseType_t xTaskCreate(void(*f)(void*),const char*n,uint32_t stack,void*a,UBaseType_t p,TaskHandle_t*out){(void)f;(void)n;(void)stack;(void)a;(void)p;*out=(void*)1;return pdPASS;}
void xTaskNotifyGive(TaskHandle_t t){assert(t&&!critical_depth);++notification_calls;}
uint32_t ulTaskNotifyTake(BaseType_t a,TickType_t t){(void)a;(void)t;return 0;}
TickType_t xTaskGetTickCount(void){return ticks;}
void vTaskDelay(TickType_t t){assert(!critical_depth);ticks+=t;if(nested_stop){nested_stop=false;bool before=auto_service;auto_service=true;assert(aether_usb_audio_playback_stop(50)==ESP_OK);auto_service=before;}if(off_before_service){off_before_service=false;atomic_store(&s.stop_requested,true);}if(auto_service)service_once();}
int64_t esp_timer_get_time(void){return (int64_t)ticks*1000;}
esp_err_t uac_host_install(const uac_host_driver_config_t*c){assert(!critical_depth&&c->create_background_task);connected_cb=c->callback;connected_arg=c->callback_arg;++install_calls;return ESP_OK;}
esp_err_t uac_host_uninstall(void){assert(!critical_depth);++uninstall_calls;if(uninstall_error)return uninstall_error;assert(!tx.opened&&!rx.opened);return ESP_OK;}
esp_err_t uac_host_device_open(const uac_host_device_config_t*c,uac_host_device_handle_t*out){assert(!critical_depth);++open_calls;struct fake_uac*d=c->iface_num==1?&tx:&rx;assert(!d->opened);d->opened=true;d->callback=c->callback;d->arg=c->callback_arg;*out=d;if(disconnect_open_phase==1){disconnect_open_phase=0;d->callback(d,UAC_HOST_DRIVER_EVENT_DISCONNECTED,d->arg);}return ESP_OK;}
esp_err_t uac_host_device_close(uac_host_device_handle_t d){assert(!critical_depth);++close_calls;if(close_error)return close_error;assert(!d->started);d->opened=false;return ESP_OK;}
esp_err_t uac_host_get_device_info(uac_host_device_handle_t d,uac_host_dev_info_t*i){assert(!critical_depth);memset(i,0,sizeof(*i));i->iface_alt_num=(ambiguous_layout||ambiguous_codec)?2:1;i->VID=0x1234;i->PID=d==&tx?1:2;if(disconnect_open_phase==2){disconnect_open_phase=0;d->callback(d,UAC_HOST_DRIVER_EVENT_DISCONNECTED,d->arg);}return ESP_OK;}
esp_err_t uac_host_get_device_alt_param(uac_host_device_handle_t d,uint8_t alt,uac_host_dev_alt_param_t*p){(void)d;assert(!critical_depth&&alt>=1&&alt<=((ambiguous_layout||ambiguous_codec)?2:1));memset(p,0,sizeof(*p));p->format=1;p->channels=mono_device?1:2;p->subframe_size=2;p->bit_resolution=bad_format?24:16;if(ambiguous_layout&&alt==1)p->subframe_size=3;if(ambiguous_codec&&alt==1)p->format=6;p->sample_freq_type=1;p->sample_freq[0]=rate44100?44100:48000;if(maximum_frequencies){p->sample_freq_type=UAC_FREQ_NUM_MAX;for(unsigned i=0;i<UAC_FREQ_NUM_MAX;++i)p->sample_freq[i]=i==UAC_FREQ_NUM_MAX-1?48000:8000+i;}if(excessive_frequencies)p->sample_freq_type=UAC_FREQ_NUM_MAX+1;if(continuous_format){p->sample_freq_type=0;p->sample_freq_lower=44100;p->sample_freq_upper=48000;}if(disconnect_open_phase==3){disconnect_open_phase=0;d->callback(d,UAC_HOST_DRIVER_EVENT_DISCONNECTED,d->arg);}return ESP_OK;}
esp_err_t uac_host_device_start(uac_host_device_handle_t d,const uac_host_stream_config_t*c){assert(!critical_depth);++start_calls;if(start_error)return start_error;assert(c->bit_resolution==16);d->rate=c->sample_freq;d->channels=c->channels;d->started=true;if(cancel_start){cancel_start=false;aether_usb_audio_class_request_stop(NULL);}return ESP_OK;}
esp_err_t uac_host_device_stop(uac_host_device_handle_t d){assert(!critical_depth);++stop_calls;if(stop_error&&(!stop_error_device||d==stop_error_device))return stop_error;d->started=false;return ESP_OK;}
esp_err_t uac_host_device_set_mute(uac_host_device_handle_t d,bool m){assert(!critical_depth&&d->started&&!m);return ESP_ERR_NOT_SUPPORTED;}
esp_err_t uac_host_device_set_volume_db(uac_host_device_handle_t d,float db){assert(!critical_depth&&d->started&&db==0.0f);return ESP_ERR_NOT_SUPPORTED;}
esp_err_t uac_host_device_write(uac_host_device_handle_t d,uint8_t*p,uint32_t n,uint32_t t){assert(!critical_depth&&d->started&&!t);if(write_error)return write_error;assert(n<=sizeof(d->tx));memcpy(d->tx,p,n);d->tx_bytes=n;return ESP_OK;}
esp_err_t uac_host_device_read(uac_host_device_handle_t d,uint8_t*p,uint32_t n,uint32_t*read,uint32_t t){assert(!critical_depth&&d->started&&!t);if(!d->rx_bytes)return ESP_ERR_TIMEOUT;if(n>d->rx_bytes)n=d->rx_bytes;memcpy(p,d->rx,n);memmove(d->rx,d->rx+n,d->rx_bytes-n);d->rx_bytes-=n;*read=n;return ESP_OK;}
static void connect(bool capture){assert(connected_cb);connected_cb(1,capture?2:1,capture?UAC_HOST_DRIVER_EVENT_RX_CONNECTED:UAC_HOST_DRIVER_EVENT_TX_CONNECTED,connected_arg);service_once();}
static void stop_class(void){aether_usb_audio_class_request_stop(NULL);service_once();assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_OK);}
static void reset_fixture(void){assert(!atomic_load(&s.installed));memset(&tx,0,sizeof(tx));memset(&rx,0,sizeof(rx));start_error=stop_error=close_error=write_error=uninstall_error=ESP_OK;bad_format=rate44100=mono_device=cancel_start=continuous_format=false;auto_service=true;disconnect_open_phase=0;stop_error_device=NULL;excessive_frequencies=maximum_frequencies=nested_stop=off_before_service=ambiguous_layout=ambiguous_codec=false;}
static void converter_tests(void)
{
    aether_pcm_converter_t a,b;int16_t input[960],whole[1200],parts[1200];
    for(unsigned i=0;i<480;++i){input[i*2]=(int16_t)(i*60-14000);input[i*2+1]=(int16_t)(14000-i*60);}
    assert(aether_pcm_converter_init(&a,48000,2,44100,2));b=a;
    size_t used,total=aether_pcm_convert(&a,input,480,whole,600,&used);assert(used==480);
    size_t n=0;for(unsigned i=0;i<480;i+=16){size_t taken;n+=aether_pcm_convert(&b,input+i*2,16,parts+n*2,600-n,&taken);assert(taken==16);}
    assert(total==n&&!memcmp(whole,parts,n*4));assert(n>=440&&n<=441);
    assert(aether_pcm_converter_init(&a,44100,1,48000,2));int16_t mono[441];for(unsigned i=0;i<441;++i)mono[i]=-32768;
    n=aether_pcm_convert(&a,mono,441,whole,600,&used);assert(used==441&&n>=479&&n<=480);
    for(size_t i=0;i<n*2;++i)assert(whole[i]==-32768);
    assert(aether_pcm_converter_init(&a,48000,2,48000,1));n=aether_pcm_convert(&a,input,480,whole,600,&used);
    for(size_t i=0;i<n;++i)assert(!whole[i]);
    assert(aether_pcm_converter_init(&a,44100,1,48000,2));n=aether_pcm_convert(&a,mono,441,whole,1,&used);assert(n<=1&&used<441);
    puts("PASS real streaming44.1/48k resampling preserves chunk-boundary phase, mono/stereo conversion and PCM16 extrema without overflow");
}
static void scheduling_tests(void)
{
    int16_t pcm[960]={0};aether_usb_audio_status_t status;
    reset_fixture();assert(worker_wait_ticks()==portMAX_DELAY);
    assert(aether_usb_audio_class_start(NULL)==ESP_OK && worker_wait_ticks()==portMAX_DELAY);
    const unsigned notice=notification_calls;
    connect(false);connect(true);assert(notification_calls>=notice+2 && worker_wait_ticks()==portMAX_DELAY);
    assert(aether_usb_audio_playback_start(100)==ESP_OK && worker_wait_ticks()==pdMS_TO_TICKS(2));
    assert(aether_usb_audio_capture_start(100)==ESP_OK && worker_wait_ticks()==pdMS_TO_TICKS(2));
    assert(aether_usb_audio_playback_stop(100)==ESP_OK && worker_wait_ticks()==pdMS_TO_TICKS(2));
    assert(aether_usb_audio_capture_stop(100)==ESP_OK && worker_wait_ticks()==portMAX_DELAY);
    const unsigned before_off=notification_calls;
    aether_usb_audio_class_request_stop(NULL);
    assert(notification_calls>before_off && worker_wait_ticks()==0);
    service_once();assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_OK && worker_wait_ticks()==portMAX_DELAY);
    puts("PASS HID-only and connected-idle UAC workers sleep on notifications; active playback/capture retain2ms and OFF wakes immediately");

    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);
    assert(aether_usb_audio_playback_start(100)==ESP_OK);
    stop_error=ESP_FAIL;aether_usb_audio_class_request_stop(NULL);service_once();
    const unsigned delays[]={10,20,40,80,160,200,200};
    for(unsigned i=0;i<sizeof(delays)/sizeof(delays[0]);++i){
        assert(worker_wait_ticks()==pdMS_TO_TICKS(delays[i]));
        const unsigned stops=stop_calls;
        for(unsigned pass=0;pass<12;++pass)service_once();
        aether_usb_audio_get_status(&status);
        assert(stop_calls==stops && atomic_load(&s.installed) && tx.opened && status.last_error==ESP_FAIL);
        assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_ERR_NOT_FINISHED);
        ticks+=pdMS_TO_TICKS(delays[i]);service_once();assert(stop_calls==stops+1);
    }
    unsigned stops=stop_calls;
    aether_usb_audio_class_request_stop(NULL);assert(worker_wait_ticks()==0);service_once();
    assert(stop_calls==stops+1 && worker_wait_ticks()==pdMS_TO_TICKS(10));
    stop_error=ESP_OK;
    tx.callback(&tx,UAC_HOST_DRIVER_EVENT_DISCONNECTED,tx.arg);
    assert(worker_wait_ticks()==0);service_once();
    assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_OK && !tx.opened);
    puts("PASS persistent cleanup keeps ownership with10/20/40/80/160/200ms retries; repeated polling cannot retry early or erase error, new OFF/disconnect bypass backoff");

    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);
    uninstall_error=ESP_FAIL;aether_usb_audio_class_request_stop(NULL);service_once();
    unsigned uninstalls=uninstall_calls;assert(worker_wait_ticks()==pdMS_TO_TICKS(10));
    for(unsigned pass=0;pass<12;++pass)service_once();
    assert(uninstall_calls==uninstalls && aether_usb_audio_class_poll_stopped(NULL)==ESP_ERR_NOT_FINISHED);
    ticks+=10;service_once();assert(uninstall_calls==uninstalls+1 && worker_wait_ticks()==pdMS_TO_TICKS(20));
    uninstall_error=ESP_OK;aether_usb_audio_class_request_stop(NULL);assert(worker_wait_ticks()==0);service_once();
    assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_OK);
    puts("PASS class-driver uninstall failure backs off without releasing Host ownership; a fresh OFF request immediately retries");

    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);connect(true);
    assert(aether_usb_audio_playback_start(100)==ESP_OK && aether_usb_audio_capture_start(100)==ESP_OK);
    stop_error=ESP_FAIL;stop_error_device=&rx;
    rx.callback(&rx,UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR,rx.arg);
    assert(aether_usb_audio_write(pcm,sizeof(pcm),10)==ESP_OK);service_once();
    assert(tx.started && tx.tx_bytes && atomic_load(&s.rx.cleanup) && worker_wait_ticks()==pdMS_TO_TICKS(2));
    const unsigned rx_stops=stop_calls;
    for(unsigned i=0;i<4;++i){ticks+=2;assert(aether_usb_audio_write(pcm,sizeof(pcm),10)==ESP_OK);service_once();}
    assert(stop_calls==rx_stops && tx.started && aether_usb_audio_playback_connected());
    stop_error=ESP_OK;rx.callback(&rx,UAC_HOST_DRIVER_EVENT_DISCONNECTED,rx.arg);service_once();
    assert(!rx.opened && tx.started && worker_wait_ticks()==pdMS_TO_TICKS(2));stop_class();
    puts("PASS one failed capture cleanup does not delay healthy playback service or PCM flow during its backoff");

    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);
    s.rx_fifo.lock->locked=true;aether_usb_audio_class_request_stop(NULL);service_once();
    assert(!tx.opened&&!rx.opened && atomic_load(&s.rx.cleanup));
    assert(worker_wait_ticks()==pdMS_TO_TICKS(10)); /* No false uninstall-ready spin. */
    s.rx_fifo.lock->locked=false;ticks+=10;service_once();
    assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_OK);
    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);
    assert(aether_usb_audio_playback_start(100)==ESP_OK);
    stop_error=ESP_FAIL;assert(aether_usb_audio_playback_stop(100)==ESP_FAIL && atomic_load(&s.tx.cleanup));
    const unsigned ordinary_stops=stop_calls;service_once();assert(stop_calls==ordinary_stops);
    stop_error=ESP_OK;ticks+=10;service_once();
    assert(!tx.started && tx.opened && !atomic_load(&s.tx.cleanup) && worker_wait_ticks()==portMAX_DELAY);stop_class();
    puts("PASS FIFO-only teardown failure sleeps until retry and ordinary failed STOP recovers without an implicit stream restart");

    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);
    assert(aether_usb_audio_playback_start(100)==ESP_OK);
    ticks=UINT32_MAX-4U;stop_error=ESP_FAIL;aether_usb_audio_class_request_stop(NULL);service_once();
    assert(worker_wait_ticks()==pdMS_TO_TICKS(10));
    const unsigned rollover_stops=stop_calls;ticks+=9;service_once();assert(stop_calls==rollover_stops && worker_wait_ticks()==1);
    stop_error=ESP_OK;++ticks;service_once();
    assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_OK);
    puts("PASS cleanup retry deadlines remain bounded across the32-bit FreeRTOS tick rollover");
}
int main(void)
{
    converter_tests();aether_usb_audio_status_t status;
    assert(aether_usb_audio_get_status(&status)==ESP_OK&&!status.installed&&!install_calls);
    assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_OK);
    assert(aether_usb_audio_init()==ESP_OK&&aether_usb_audio_init()==ESP_OK&&!install_calls);
    assert(!aether_usb_audio_playback_connected()&&!aether_usb_audio_capture_connected());
    assert(aether_usb_audio_playback_start(10)==ESP_ERR_INVALID_STATE);
    puts("PASS lazy idempotent initialization and cache-only status/connection checks perform no host-core/client/power operations");
    assert(aether_usb_audio_class_start(NULL)==ESP_OK&&install_calls==1);connect(false);connect(true);
    assert(aether_usb_audio_playback_connected()&&aether_usb_audio_capture_connected());
    assert(aether_usb_audio_playback_start(100)==ESP_OK&&tx.started);
    assert(aether_usb_audio_capture_start(100)==ESP_OK&&rx.started);
    int16_t pcm[960];for(unsigned i=0;i<960;++i)pcm[i]=(int16_t)(i-480);
    assert(aether_usb_audio_write(pcm,sizeof(pcm),10)==ESP_OK);service_once();assert(tx.tx_bytes==479*4&&!memcmp(tx.tx,pcm,tx.tx_bytes));
    memcpy(rx.rx,pcm,sizeof(pcm));rx.rx_bytes=sizeof(pcm);service_once();int16_t captured[200];
    assert(aether_usb_audio_capture_read(captured,sizeof(captured),10)==ESP_OK&&!memcmp(captured,pcm,sizeof(captured)));
    assert(aether_usb_audio_playback_stop(100)==ESP_OK&&aether_usb_audio_playback_stop(100)==ESP_OK);
    assert(aether_usb_audio_capture_stop(100)==ESP_OK);stop_class();
    puts("PASS actual UAC worker negotiates PCM16 endpoints and moves speaker+microphone data through normalized whole-block FIFOs; stop is idempotent");
    reset_fixture();rate44100=mono_device=true;assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);connect(true);
    assert(aether_usb_audio_playback_start(100)==ESP_OK&&tx.rate==44100&&tx.channels==1);
    assert(aether_usb_audio_capture_start(100)==ESP_OK&&rx.rate==44100&&rx.channels==1);
    assert(aether_usb_audio_write(pcm,sizeof(pcm),10)==ESP_OK);service_once();assert(tx.tx_bytes>=440*2&&tx.tx_bytes<=441*2);
    stop_class();reset_fixture();bad_format=true;assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);
    assert(!aether_usb_audio_playback_connected()&&!tx.opened);aether_usb_audio_get_status(&status);assert(status.unsupported_interfaces&&status.playback_error==ESP_ERR_NOT_SUPPORTED);
    stop_class();puts("PASS44.1k-only mono endpoints are genuinely resampled; unsupported24bit formats close cleanly and report NOT_SUPPORTED");
    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);auto_service=false;
    assert(aether_usb_audio_playback_start(5)==ESP_ERR_TIMEOUT&&!atomic_load(&s.tx.wanted));auto_service=true;service_once();assert(!tx.started);
    cancel_start=true;assert(aether_usb_audio_playback_start(100)!=ESP_OK);service_once();assert(!tx.started&&!atomic_load(&s.installed));
    puts("PASS timed-out or concurrent Host-OFF starts are cancelled and cannot publish a late active stream");
    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);assert(aether_usb_audio_playback_start(100)==ESP_OK);
    stop_error=ESP_FAIL;assert(aether_usb_audio_playback_stop(100)==ESP_FAIL&&tx.started&&atomic_load(&s.tx.cleanup));
    assert(aether_usb_audio_write(pcm,sizeof(pcm),0)==ESP_ERR_INVALID_STATE);aether_usb_audio_class_request_stop(NULL);service_once();
    assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_ERR_NOT_FINISHED&&tx.opened);
    stop_error=ESP_OK;ticks+=10;service_once();assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_OK&&!tx.opened);
    puts("PASS failed stop retains UAC/DMA ownership, blocks fresh writes and prevents Host release until a successful cleanup retry");
    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);assert(aether_usb_audio_playback_start(100)==ESP_OK);
    write_error=ESP_FAIL;assert(aether_usb_audio_write(pcm,sizeof(pcm),10)==ESP_OK);service_once();assert(s.tx_pending);
    tx.callback(&tx,UAC_HOST_DRIVER_EVENT_DISCONNECTED,tx.arg);assert(!aether_usb_audio_playback_connected());service_once();assert(!tx.opened&&!s.tx_pending);
    stop_class();puts("PASS disconnect immediately gates I/O and releases queued/staged PCM only after official stream/device teardown");
    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);assert(aether_usb_audio_playback_start(100)==ESP_OK);auto_service=false;
    uint8_t *big=calloc(1,PCM_FIFO_SIZE);assert(aether_usb_audio_write(big,PCM_FIFO_SIZE,0)==ESP_OK);
    const size_t before=s.tx_fifo.used;assert(aether_usb_audio_write(pcm,4,0)==ESP_ERR_TIMEOUT&&s.tx_fifo.used==before);
    auto_service=true;free(big);stop_class();
    puts("PASS FIFO backpressure is bounded and whole-block atomic; timeout cannot silently consume or acknowledge a partial PCM block");
    assert(aether_usb_audio_playback_stop(0)==ESP_OK&&aether_usb_audio_capture_stop(0)==ESP_OK);
    reset_fixture();continuous_format=true;assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);connect(true);
    assert(aether_usb_audio_playback_start(100)==ESP_OK&&tx.rate==48000);
    start_error=ESP_FAIL;const unsigned prior_stop=stop_calls;
    assert(aether_usb_audio_capture_start(100)==ESP_FAIL&&!rx.started&&stop_calls>prior_stop&&!s.rx.stream_resources);
    start_error=ESP_OK;assert(aether_usb_audio_capture_start(100)==ESP_OK);
    memcpy(rx.rx,pcm,sizeof(pcm));rx.rx_bytes=sizeof(pcm);s.rx_fifo.used=PCM_FIFO_SIZE;
    service_once();assert(atomic_load(&s.rx.transfer_error));service_once();
    aether_usb_audio_get_status(&status);assert(status.rx_overruns&&status.capture_error==ESP_ERR_NO_MEM&&!rx.opened);
    assert(aether_usb_audio_capture_stop(100)==ESP_OK);
    uninstall_error=ESP_FAIL;aether_usb_audio_class_request_stop(NULL);service_once();
    assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_ERR_NOT_FINISHED);
    uninstall_error=ESP_OK;ticks+=10;service_once();assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_OK);
    puts("PASS continuous-rate negotiation chooses48k; failed start cleans possible stream resources, capture overflow is explicit, and client-uninstall failure keeps Host ownership pending");

    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);assert(aether_usb_audio_playback_start(100)==ESP_OK);
    close_error=ESP_FAIL;aether_usb_audio_class_request_stop(NULL);service_once();
    assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_ERR_NOT_FINISHED&&tx.opened);
    close_error=ESP_OK;ticks+=10;service_once();assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_OK);
    assert(aether_usb_audio_playback_stop(0)==ESP_OK);
    puts("PASS failed device close retains the class client until retry; stop after Host OFF is immediately idempotent without USB calls");
    for(unsigned phase=1;phase<=3;++phase){
        reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);disconnect_open_phase=phase;connect(false);
        assert(!aether_usb_audio_playback_connected()&&!tx.opened&&!s.tx.device);
        aether_usb_audio_get_status(&status);assert(!status.playback_ready&&!status.playback_connected);
        assert(aether_usb_audio_playback_start(10)==ESP_ERR_INVALID_STATE);stop_class();
    }
    puts("PASS disconnect during open, device-info or alternate negotiation cannot be overwritten by late availability; checked close and class teardown complete");
    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);excessive_frequencies=true;const unsigned before_start=start_calls;connect(false);
    assert(!aether_usb_audio_playback_connected()&&!tx.opened&&start_calls==before_start);stop_class();
    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);assert(aether_usb_audio_playback_start(100)==ESP_OK);
    auto_service=false;nested_stop=true;assert(aether_usb_audio_playback_stop(100)==ESP_OK&&!tx.started);auto_service=true;stop_class();
    puts("PASS descriptor frequency counts beyond upstream storage capacity are refused before start; concurrent STOP callers share one acknowledged cleanup");
    reset_fixture();maximum_frequencies=true;assert(UAC_FREQ_NUM_MAX==32);assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);
    assert(aether_usb_audio_playback_connected()&&aether_usb_audio_playback_start(100)==ESP_OK&&tx.rate==48000);stop_class();
    reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);connect(false);off_before_service=true;
    assert(aether_usb_audio_playback_start(100)==ESP_ERR_INVALID_STATE&&!tx.started);service_once();assert(aether_usb_audio_class_poll_stopped(NULL)==ESP_OK);
    puts("PASS all32 advertised frequencies are safely selectable; OFF before START service cannot return a successful STOP acknowledgment as START");
    for(unsigned kind=0;kind<2;++kind){
        reset_fixture();assert(aether_usb_audio_class_start(NULL)==ESP_OK);ambiguous_layout=kind==0;ambiguous_codec=kind==1;
        const unsigned before=start_calls;connect(false);assert(!aether_usb_audio_playback_connected()&&!tx.opened&&start_calls==before);
        aether_usb_audio_get_status(&status);assert(status.playback_error==ESP_ERR_NOT_SUPPORTED);stop_class();
    }
    puts("PASS padded/non-PCM alternates sharing a packed-PCM16 rate tuple are rejected before official selection can choose an incompatible wire layout");
    scheduling_tests();
    assert(!critical_depth);free_initial_resources();s.initialized=false;
    return 0;
}
