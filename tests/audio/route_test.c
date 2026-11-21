/* Real pthread contention against the production codec route/session code.
 * The transport models delayed media ACKs and STOP racing before START. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include "../../components/ac101s_audio/ac101s_audio.c"

struct test_mutex { pthread_mutex_t value; };
static atomic_bool sink_entered, sink_cancelled, sink_release, late_request;
static atomic_int route_result, write_result, stop_result;
static atomic_bool gate_stop,stop_gate_entered,release_stop_gate;
static _Thread_local bool stopping_thread;
static bool codec_open, muted;
static int pa;
static uint64_t stop_elapsed;
static void delay_ms(unsigned ms)
{ struct timespec t={ms/1000,(long)(ms%1000)*1000000}; nanosleep(&t,NULL); }
static uint64_t now_ms(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)now_ms(); }
int64_t esp_timer_get_time(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t timeout)
{
    if(stopping_thread && atomic_exchange(&gate_stop,false)) {
        atomic_store(&stop_gate_entered,true);
        while(!atomic_load(&release_stop_gate)) delay_ms(1);
    }
    struct timespec end; clock_gettime(CLOCK_REALTIME,&end);
    end.tv_sec+=timeout/1000; end.tv_nsec+=(long)(timeout%1000)*1000000;
    end.tv_sec+=end.tv_nsec/1000000000; end.tv_nsec%=1000000000;
    int result=pthread_mutex_timedlock(&h->value,&end);
    assert(result==0 || result==ETIMEDOUT); return result==0 ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { assert(!pthread_mutex_unlock(&h->value)); return pdTRUE; }
esp_err_t gpio_set_level(gpio_num_t pin,uint32_t level) { assert(pin==42); pa=(int)level; return ESP_OK; }
esp_err_t gpio_input_enable(gpio_num_t pin) { assert(pin==42); return ESP_OK; }
esp_err_t gpio_set_direction(gpio_num_t pin,int mode) { assert(pin==42 && mode==GPIO_MODE_OUTPUT); return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { assert(pin==42); return pa; }
esp_err_t ac101s_codec_start(i2c_master_dev_handle_t dev, bool capture_only, float gain_db)
{
    assert(dev && !capture_only && gain_db == 24 && !pa);
    codec_open = muted = true;
    return ESP_OK;
}

esp_err_t ac101s_codec_stop(i2c_master_dev_handle_t dev)
{
    assert(dev && !pa);
    codec_open = false;
    return ESP_OK;
}

esp_err_t ac101s_codec_set_speaker(i2c_master_dev_handle_t dev, unsigned volume)
{
    assert(dev && volume <= 70 && !pa);
    muted = volume == 0;
    return ESP_OK;
}

int i2s_channel_write(i2s_chan_handle_t tx, const void *data, size_t bytes, size_t *sent, unsigned timeout)
{
    assert(tx && data && bytes == 4 && timeout && codec_open && !muted);
    *sent = bytes;
    return ESP_OK;
}

static bool connected(void *context) { assert(context==(void*)1); return true; }
static esp_err_t transport_stop(void *context)
{ assert(context==(void*)1);atomic_store(&sink_cancelled,true);return ESP_OK; }
static esp_err_t transport_start(void *context)
{
    assert(context==(void*)1 && !pa && !codec_open);
    atomic_store(&sink_entered,true);
    if(atomic_load(&late_request)) {
        /* STOP happens during this gap; the SDK's later request publication
         * must be cancelled again before its eventual ACK can win. */
        delay_ms(80); atomic_store(&sink_cancelled,false);
    }
    const uint64_t deadline=now_ms()+3000;
    while(!atomic_load(&sink_release)&&!atomic_load(&sink_cancelled)&&now_ms()<deadline) delay_ms(1);
    return atomic_load(&sink_cancelled) ? ESP_ERR_INVALID_STATE : atomic_load(&sink_release) ? ESP_OK : ESP_ERR_TIMEOUT;
}
static esp_err_t transport_write(void *context,const void *pcm,size_t bytes,uint32_t timeout)
{ assert(context==(void*)1 && pcm && bytes==4 && timeout && !pa && !codec_open); return ESP_OK; }
static void *route_thread(void *context)
{ atomic_store(&route_result,ac101s_audio_set_output(context,AC101S_OUTPUT_EXTERNAL)); return NULL; }
static void *write_thread(void *context)
{ int16_t pcm[2]={100,-100};atomic_store(&write_result,ac101s_audio_write(context,pcm,sizeof(pcm)));return NULL; }
static void *stop_thread(void *context)
{ stopping_thread=true;uint64_t before=now_ms();atomic_store(&stop_result,ac101s_audio_stop(context));stop_elapsed=now_ms()-before;return NULL; }
static void reset_handshake(bool late)
{
    atomic_store(&sink_entered,false);atomic_store(&sink_cancelled,false);atomic_store(&sink_release,false);
    atomic_store(&late_request,late);atomic_store(&route_result,123);atomic_store(&write_result,123);atomic_store(&stop_result,123);
}
static void wait_entered(void)
{ for(unsigned i=0;i<1000&&!atomic_load(&sink_entered);++i)delay_ms(1);assert(atomic_load(&sink_entered)); }
int main(void)
{
    struct test_mutex mutex={.value=PTHREAD_MUTEX_INITIALIZER};
    struct ac101s_audio_dev_t audio={.codec=(void*)1,.tx=(void*)2,.pa_enable=42,.sample_rate_hz=48000,
        .master_volume=100,.mutex=&mutex,.route_lock=portMUX_INITIALIZER_UNLOCKED};
    ac101s_audio_sink_t sink={.context=(void*)1,.connected=connected,.start=transport_start,
        .stop=transport_stop,.write=transport_write};
    assert(ac101s_audio_set_external_sink(&audio,&sink)==ESP_OK);
    assert(ac101s_audio_start(&audio,70,24)==ESP_OK);
    pthread_t route,writer,stopper;
    reset_handshake(false);assert(!pthread_create(&route,NULL,route_thread,&audio));wait_entered();
    assert(!pthread_create(&writer,NULL,write_thread,&audio));delay_ms(1200);
    assert(atomic_load(&write_result)==123); /* Original 1s wait must not fail. */
    delay_ms(800);atomic_store(&sink_release,true);
    pthread_join(route,NULL);pthread_join(writer,NULL);
    assert(atomic_load(&route_result)==ESP_OK&&atomic_load(&write_result)==ESP_OK);
    assert(audio.started&&audio.external_started&&!pa&&!codec_open);
    puts("PASS production route + concurrent writer survives delayed 2s transport start; no normal I/O timeout");

    assert(ac101s_audio_stop(&audio)==ESP_OK);
    assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_SPEAKER)==ESP_OK);
    assert(ac101s_audio_start(&audio,70,24)==ESP_OK);
    reset_handshake(true);assert(!pthread_create(&route,NULL,route_thread,&audio));wait_entered();
    assert(!pthread_create(&stopper,NULL,stop_thread,&audio));
    pthread_join(stopper,NULL);pthread_join(route,NULL);
    assert(atomic_load(&stop_result)==ESP_OK&&stop_elapsed<1000);
    assert(atomic_load(&route_result)==ESP_ERR_INVALID_STATE&&!audio.started&&!audio.external_started&&!pa&&!codec_open);
    atomic_store(&sink_release,true);delay_ms(30);
    assert(!audio.started&&!pa); /* Late ACK cannot restart either old or new route. */
    puts("PASS STOP cancels route handshake within existing 1s stop bound, including late request publication; no rollback/reopen after STOP");

    assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_SPEAKER)==ESP_OK);
    assert(ac101s_audio_start(&audio,70,24)==ESP_OK);
    reset_handshake(false);atomic_store(&stop_gate_entered,false);atomic_store(&release_stop_gate,false);atomic_store(&gate_stop,true);
    assert(!pthread_create(&stopper,NULL,stop_thread,&audio));
    for(unsigned i=0;i<1000&&!atomic_load(&stop_gate_entered);++i) delay_ms(1);
    assert(atomic_load(&stop_gate_entered));
    /* STOP has incremented its serial and observed NULL cancellation, but
     * a new route gets mutex first and captures that same newer serial. */
    assert(!pthread_create(&route,NULL,route_thread,&audio));pthread_join(route,NULL);
    assert(atomic_load(&route_result)==ESP_ERR_INVALID_STATE&&!atomic_load(&sink_entered)&&!audio.started&&!pa);
    atomic_store(&release_stop_gate,true);pthread_join(stopper,NULL);
    assert(atomic_load(&stop_result)==ESP_OK&&!audio.started&&!audio.external_started&&!pa);
    puts("PASS STOP-first NULL cancellation snapshot: competing route cannot start or roll back an unfinished STOP even with matching new serial");

    assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_SPEAKER)==ESP_OK);
    assert(ac101s_audio_start(&audio,70,24)==ESP_OK);
    assert(xSemaphoreTake(audio.mutex,0)==pdTRUE);atomic_store(&write_result,123);
    uint64_t began=now_ms();assert(!pthread_create(&writer,NULL,write_thread,&audio));pthread_join(writer,NULL);
    uint64_t elapsed=now_ms()-began;assert(atomic_load(&write_result)==ESP_ERR_TIMEOUT&&elapsed>=900&&elapsed<2000);
    xSemaphoreGive(audio.mutex);assert(ac101s_audio_stop(&audio)==ESP_OK);
    assert(!(atomic_load(&audio.route_epoch)&1U));
    pthread_mutex_destroy(&mutex.value);pthread_mutex_destroy(&audio.route_lock);
    puts("PASS unrelated mutex contention retains normal 1s I/O bound; completed route epoch is not a permanent timeout extension");
    return 0;
}
