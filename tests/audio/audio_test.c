/* Exercise production session code. Hardware/codec stubs retain mute across
 * sessions and assert PA remains off during setup. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../components/ac101s_audio/ac101s_audio.c"

struct test_mutex { bool locked; };
static int pa;
static bool codec_open, muted, bad_order;
static int fail_stage, stage, close_calls;
static int actual_volume, fail_volumes;
static bool fail_close;
static bool check_local_pcm;
static uint32_t local_pcm;
static TickType_t ticks, lock_ticks, local_write_ticks;
static int local_write_error;
esp_err_t gpio_input_enable(gpio_num_t pin) { assert(pin == 42); return ESP_OK; }
esp_err_t gpio_set_direction(gpio_num_t pin, int mode) { assert(pin == 42 && mode == GPIO_MODE_OUTPUT); return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { assert(pin == 42); return pa; }
TickType_t xTaskGetTickCount(void) { return ticks; }
int64_t esp_timer_get_time(void) { return (int64_t)ticks * 1000; }

BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout)
{
    if (mutex->locked) { ticks+=timeout; return pdFALSE; }
    ticks += lock_ticks; lock_ticks = 0;
    mutex->locked = true;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex->locked);
    mutex->locked = false;
    return pdTRUE;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    assert(pin == 42);
    if (level && (!codec_open || muted || stage < 2)) bad_order = true;
    pa = (int)level;
    return ESP_OK;
}
esp_err_t ac101s_codec_start(i2c_master_dev_handle_t dev, bool capture_only, float gain_db)
{
    assert(dev && !capture_only && gain_db == 24.0f);
    if (pa) bad_order = true;
    codec_open = true;
    muted = true;
    return ++stage == fail_stage ? ESP_FAIL : ESP_OK;
}

esp_err_t ac101s_codec_stop(i2c_master_dev_handle_t dev)
{
    assert(dev);
    if (pa) bad_order = true;
    if (fail_close) return ESP_FAIL;
    codec_open = false;
    muted = true;
    ++close_calls;
    return ESP_OK;
}

esp_err_t ac101s_codec_set_speaker(i2c_master_dev_handle_t dev, unsigned volume)
{
    assert(dev && volume <= 70);
    if (pa) bad_order = true;
    ++stage;
    if (fail_volumes) { --fail_volumes; return ESP_FAIL; }
    if (fail_stage == stage) return ESP_FAIL;
    actual_volume = (int)volume;
    muted = volume == 0;
    return ESP_OK;
}

int i2s_channel_write(i2s_chan_handle_t tx, const void *data, size_t size, size_t *sent, unsigned timeout)
{
    assert(tx && size == 4 && timeout <= 1000);
    ticks += local_write_ticks;
    if (check_local_pcm) memcpy(&local_pcm, data, 4);
    *sent = size;
    return local_write_error;
}

int i2s_channel_read(i2s_chan_handle_t rx, void *data, size_t size, size_t *received, unsigned timeout)
{
    assert(rx && size == 4 && timeout <= 1000);
    memset(data, 0, size);
    *received = size;
    return ESP_OK;
}

static bool peer_connected = true;
static unsigned sink_starts, sink_stops, sink_writes, fail_sink_write;
static bool fail_sink_start;
static unsigned fail_sink_stop;
static ac101s_audio_handle_t publish_stop_during_cleanup;
static TickType_t sink_ticks;
static int16_t captured[4096];
static size_t captured_samples;
static esp_err_t transport_start(void *context)
{ assert(context == &peer_connected && !pa && !codec_open); ++sink_starts; return fail_sink_start ? ESP_FAIL : ESP_OK; }
static esp_err_t transport_stop(void *context)
{ assert(context == &peer_connected && !pa); ++sink_stops;
  if(publish_stop_during_cleanup){atomic_fetch_add(&publish_stop_during_cleanup->stop_serial,1);publish_stop_during_cleanup=NULL;}
  if(fail_sink_stop){--fail_sink_stop;return ESP_FAIL;}return ESP_OK; }
static bool transport_connected(void *context) { assert(context == &peer_connected); return peer_connected; }
static esp_err_t transport_write(void *context, const void *data, size_t bytes, uint32_t timeout)
{
    assert(context == &peer_connected && !pa && bytes <= 1024 && timeout <= 1000 && timeout);
    ++sink_writes; ticks += sink_ticks;
    if (fail_sink_write == sink_writes) return ESP_ERR_TIMEOUT;
    assert(captured_samples + bytes/2 <= sizeof(captured)/sizeof(*captured));
    memcpy(captured + captured_samples,data,bytes); captured_samples += bytes/2;
    return ESP_OK;
}

int main(void)
{
    struct test_mutex mutex = {0};
    struct ac101s_audio_dev_t audio = {.codec = (void *)1, .tx = (void *)2, .rx = (void *)3, .pa_enable = 42,
                                     .sample_rate_hz = 48000, .mutex = &mutex, .master_volume = 100,
                                     .route_lock = portMUX_INITIALIZER_UNLOCKED};
    uint32_t frame = 0;
    ac101s_audio_write_metrics_t initial_metrics;
    assert(ac101s_audio_get_write_metrics(&audio,&initial_metrics)==ESP_OK);
    assert(!initial_metrics.write_calls && !initial_metrics.mutex_wait_max_us && !initial_metrics.output_write_max_us);
    /* Repeated sessions explicitly clear the persisted mute and gate PA. */
    for (unsigned session = 0; session < 100; ++session) {
        stage = 0;
        assert(ac101s_audio_start(&audio, 70, 24.0f) == ESP_OK);
        assert(actual_volume == 49);
        assert(pa == 1 && !muted && audio.started && !bad_order);
        assert(ac101s_audio_start(&audio, 70, 24.0f) == ESP_ERR_INVALID_STATE);
        assert(pa == 1 && !muted);
        assert(ac101s_audio_write(&audio, &frame, 4) == ESP_OK);
        assert(ac101s_audio_read(&audio, &frame, 4) == ESP_OK);
        assert(ac101s_audio_stop(&audio) == ESP_OK);
        assert(!pa && muted && !codec_open && !audio.started && !mutex.locked);
    }
    assert(close_calls == 100 && !bad_order);
    puts("PASS 100 codec restart/mute/PA sequencing cycles");

    for (fail_stage = 1; fail_stage <= 2; ++fail_stage) {
        stage = 0;
        assert(ac101s_audio_start(&audio, 70, 24.0f) == ESP_FAIL);
        assert(!pa && !codec_open && !audio.started && !mutex.locked && !bad_order);
    }
    fail_stage = 0;
    assert(close_calls == 102);
    puts("PASS codec start and volume application failure rollback");

    assert(ac101s_audio_start(&audio, -1, 24.0f) == ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_start(&audio, 101, 24.0f) == ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_set_master_volume(&audio,101)==ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_set_master_volume(&audio,255)==ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_start(&audio, 70, NAN) == ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_write(&audio, &frame, 3) == ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_read(&audio, &frame, (size_t)INT_MAX + 1) == ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_write(&audio, &frame, 4) == ESP_ERR_INVALID_STATE);
    mutex.locked = true;
    assert(ac101s_audio_start(&audio, 70, 24.0f) == ESP_ERR_TIMEOUT);
    assert(ac101s_audio_stop(&audio) == ESP_ERR_TIMEOUT);
    mutex.locked = false;
    assert(!pa && !bad_order);
    puts("PASS validation and mutex timeout paths");

    ac101s_audio_write_metrics_t before, metrics;
    assert(ac101s_audio_get_write_metrics(NULL,&metrics)==ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_get_write_metrics(&audio,NULL)==ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_get_write_metrics(&audio,&before)==ESP_OK);
    stage=0; assert(ac101s_audio_start(&audio,70,24)==ESP_OK);
    lock_ticks=17; local_write_ticks=23;
    assert(ac101s_audio_write(&audio,&frame,4)==ESP_OK);
    assert(ac101s_audio_get_write_metrics(&audio,&metrics)==ESP_OK);
    assert(metrics.write_calls==before.write_calls+1 && metrics.last_error==ESP_OK);
    assert(metrics.mutex_wait_us==17000 && metrics.mutex_wait_max_us==17000);
    assert(metrics.output_write_us==23000 && metrics.output_write_max_us==23000);
    mutex.locked=true; const TickType_t getter_at=ticks;
    assert(ac101s_audio_get_write_metrics(&audio,&metrics)==ESP_OK && ticks==getter_at);
    assert(ac101s_audio_write(&audio,&frame,4)==ESP_ERR_TIMEOUT);
    mutex.locked=false;
    assert(ac101s_audio_get_write_metrics(&audio,&metrics)==ESP_OK);
    assert(metrics.mutex_wait_us==1000000 && metrics.mutex_wait_max_us==1000000);
    assert(!metrics.output_write_us && metrics.output_write_max_us==23000);
    assert(metrics.mutex_timeouts==before.mutex_timeouts+1 && metrics.last_error==ESP_ERR_TIMEOUT);
    local_write_ticks=31; local_write_error=ESP_FAIL;
    assert(ac101s_audio_write(&audio,&frame,4)==ESP_FAIL);
    assert(ac101s_audio_get_write_metrics(&audio,&metrics)==ESP_OK);
    assert(!metrics.mutex_wait_us && metrics.output_write_us==31000 && metrics.output_write_max_us==31000);
    assert(metrics.output_errors==before.output_errors+1 && metrics.last_error==ESP_FAIL);
    local_write_ticks=0; local_write_error=ESP_OK;
    assert(ac101s_audio_write(&audio,&frame,4)==ESP_OK);
    assert(ac101s_audio_get_write_metrics(&audio,&metrics)==ESP_OK);
    assert(metrics.write_calls==before.write_calls+4 && !metrics.output_write_us && metrics.output_write_max_us==31000);
    assert(metrics.mutex_wait_max_us==1000000 && metrics.mutex_timeouts==before.mutex_timeouts+1);
    assert(metrics.output_errors==before.output_errors+1 && metrics.last_error==ESP_OK);
    assert(ac101s_audio_stop(&audio)==ESP_OK);
    puts("PASS cached write metrics separate mutex wait/output time, retain timeout and output-failure peaks, and read without acquiring the held audio mutex");

    /* Master zero must actually mute and gate PA, including restarts. */
    stage=0; assert(ac101s_audio_start(&audio,70,24)==ESP_OK);
    assert(ac101s_audio_set_master_volume(&audio,50)==ESP_OK && actual_volume==25 && pa);
    assert(ac101s_audio_set_master_volume(&audio,0)==ESP_OK && actual_volume==0 && muted && !pa);
    assert(ac101s_audio_stop(&audio)==ESP_OK);
    stage=0; assert(ac101s_audio_start(&audio,70,24)==ESP_OK && !pa && muted && !actual_volume);
    assert(ac101s_audio_set_master_volume(&audio,100)==ESP_OK && pa && !muted && actual_volume==49);
    fail_volumes=1; assert(ac101s_audio_set_master_volume(&audio,25)==ESP_FAIL);
    assert(audio.master_volume==100 && actual_volume==49 && pa && !muted);
    fail_volumes=2; assert(ac101s_audio_set_master_volume(&audio,25)==ESP_FAIL && !pa);
    assert(ac101s_audio_stop(&audio)==ESP_OK);
    puts("PASS master volume, true mute, restart and failed volume rollback");

    assert(ac101s_audio_start(&audio,70,24)==ESP_OK && pa);
    fail_close = true;
    assert(ac101s_audio_stop(&audio)==ESP_FAIL && audio.started && !pa);
    assert(ac101s_audio_set_master_volume(&audio,75)==ESP_OK && !pa);
    assert(ac101s_audio_write(&audio,&frame,4)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_start(&audio,70,24)==ESP_ERR_INVALID_STATE);
    fail_close = false;
    assert(ac101s_audio_stop(&audio)==ESP_OK && !audio.started && !pa);
    assert(ac101s_audio_set_master_volume(&audio,100)==ESP_OK);
    puts("PASS failed local STOP retains ownership, blocks PCM and keeps PA disabled");

    ac101s_audio_sink_t sink={.context=&peer_connected,.start=transport_start,.stop=transport_stop,
        .write=transport_write,.connected=transport_connected};
    assert(ac101s_audio_set_external_sink(&audio,&sink)==ESP_OK);
    stage=0; assert(ac101s_audio_start(&audio,70,24)==ESP_OK);
    fail_sink_start=true;
    assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_EXTERNAL)==ESP_FAIL);
    assert(audio.output==AC101S_OUTPUT_SPEAKER && audio.started && codec_open && pa && sink_stops==1);
    fail_sink_start=false;
    assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_EXTERNAL)==ESP_OK && !pa && !codec_open && audio.external_started);
    assert(ac101s_audio_read(&audio,&frame,4)==ESP_ERR_NOT_SUPPORTED);
    assert(ac101s_audio_set_external_sink(&audio,NULL)==ESP_ERR_INVALID_STATE);
    int16_t pcm[1500], original[1500];
    for (unsigned i=0;i<1500;++i) pcm[i]=(int16_t)((int)i*41-30000);
    memcpy(original,pcm,sizeof(pcm)); captured_samples=0;
    assert(ac101s_audio_set_master_volume(&audio,50)==ESP_OK);
    assert(ac101s_audio_write(&audio,pcm,sizeof(pcm))==ESP_OK && captured_samples==1500);
    assert(!memcmp(original,pcm,sizeof(pcm)));
    for(unsigned i=0;i<1500;++i) assert(captured[i]==(int16_t)((int32_t)original[i]*1225/10000));
    captured_samples=0; assert(ac101s_audio_set_master_volume(&audio,0)==ESP_OK);
    assert(ac101s_audio_write(&audio,pcm,sizeof(pcm))==ESP_OK);
    for(unsigned i=0;i<1500;++i) assert(!captured[i]);
    assert(!memcmp(original,pcm,sizeof(pcm)) && !pa);
    sink_ticks=600;captured_samples=0;
    assert(ac101s_audio_write(&audio,pcm,sizeof(pcm))==ESP_ERR_TIMEOUT);
    sink_ticks=0;fail_sink_write=sink_writes+1;
    assert(ac101s_audio_write(&audio,pcm,sizeof(pcm))==ESP_ERR_TIMEOUT);
    fail_sink_write=0;peer_connected=false;
    assert(ac101s_audio_write(&audio,pcm,sizeof(pcm))==ESP_ERR_INVALID_STATE && audio.output==AC101S_OUTPUT_EXTERNAL && !pa);
    assert(ac101s_audio_stop(&audio)==ESP_OK);
    assert(ac101s_audio_start(&audio,70,24)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_SPEAKER)==ESP_OK && !pa);
    assert(ac101s_audio_set_master_volume(&audio,100)==ESP_OK);
    stage=0;assert(ac101s_audio_start(&audio,70,24)==ESP_OK && pa);
    assert(ac101s_audio_stop(&audio)==ESP_OK && !bad_order);
    puts("PASS external route rollback, source-preserving PCM/mute, write deadline, disconnect retains route and manual speaker recovery");

    /* Exercise the actual start/mute/PA and live master-update paths across
     * every legal pair, rather than checking only a duplicated helper. */
    for(unsigned stream=0;stream<=100;++stream){
        assert(ac101s_audio_set_master_volume(&audio,100)==ESP_OK);
        stage=0;assert(ac101s_audio_start(&audio,(int)stream,24)==ESP_OK);
        for(unsigned master=0;master<=100;++master){
            assert(ac101s_audio_set_master_volume(&audio,(uint8_t)master)==ESP_OK);
            const unsigned old_level=stream*master/100;
            assert(actual_volume<=70);
            /* Ceiling division characterized without copying its expression. */
            assert((unsigned)actual_volume*100>=old_level*70);
            if(actual_volume) assert((unsigned)(actual_volume-1)*100<old_level*70);
            assert((actual_volume>0)==(old_level>0));
            assert((bool)pa==(old_level>0)&&muted==(old_level==0));
            uint8_t logical=255;assert(ac101s_audio_get_master_volume(&audio,&logical)==ESP_OK&&logical==master);
        }
        assert(ac101s_audio_stop(&audio)==ESP_OK&&!pa&&muted);
    }
    puts("PASS 10,201 real stream/master combinations: codec <=70, logical getter unchanged, no new low-volume mute, zero PA off");

    /* A persisted logical 100 is reapplied unchanged by the service. Neither
     * restarting nor routing back from headphones may bypass the ceiling. */
    peer_connected=true;assert(ac101s_audio_set_master_volume(&audio,100)==ESP_OK);
    for(unsigned session=0;session<100;++session){
        stage=0;assert(ac101s_audio_start(&audio,100,24)==ESP_OK&&actual_volume==70&&pa&&!muted);
        assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_EXTERNAL)==ESP_OK&&!pa&&!codec_open);
        int16_t full_scale[]={INT16_MIN,INT16_MAX,-1,1};captured_samples=0;
        assert(ac101s_audio_write(&audio,full_scale,sizeof(full_scale))==ESP_OK&&captured_samples==4);
        assert(!memcmp(captured,full_scale,sizeof(full_scale)));
        assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_SPEAKER)==ESP_OK&&actual_volume==70&&pa&&!muted);
        assert(ac101s_audio_stop(&audio)==ESP_OK&&!pa&&muted);
    }
    assert(!bad_order);
    puts("PASS saved logical100 / stream100: 100 restarts and BT round trips capped at70; external full-scale PCM unchanged");

    assert(ac101s_audio_set_usb_sink(&audio,&sink)==ESP_OK);
    for(unsigned repeat=0;repeat<20;++repeat){
        stage=0;assert(ac101s_audio_start(&audio,100,24)==ESP_OK&&actual_volume==70);
        assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_USB)==ESP_OK&&!pa);
        int16_t pcm[]={INT16_MIN,INT16_MAX};captured_samples=0;
        assert(ac101s_audio_write(&audio,pcm,4)==ESP_OK&&captured_samples==2&&!memcmp(captured,pcm,4));
        assert(ac101s_audio_set_master_volume(&audio,50)==ESP_OK);captured_samples=0;
        assert(ac101s_audio_write(&audio,pcm,4)==ESP_OK&&captured[0]==-8192&&captured[1]==8191);
        assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_EXTERNAL)==ESP_OK&&!pa);
        assert(ac101s_audio_write(&audio,pcm,4)==ESP_OK);
        assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_USB)==ESP_OK);
        /* Removal is handled on the write task before a decoder can see it;
         * speaker receives original PCM, with gain applied by local hardware. */
        peer_connected=false;check_local_pcm=true;
        assert(ac101s_audio_write(&audio,pcm,4)==ESP_OK&&audio.started&&!audio.external_started);
        assert(audio.output==AC101S_OUTPUT_SPEAKER&&local_pcm==0x7fff8000U&&actual_volume==35&&pa);
        check_local_pcm=false;peer_connected=true;
        assert(ac101s_audio_stop(&audio)==ESP_OK);
        assert(ac101s_audio_set_master_volume(&audio,100)==ESP_OK);
    }
    assert(!bad_order);
    puts("PASS 20 live speaker/USB/BT/USB transitions: single software gain, unchanged full-scale PCM, immediate USB removal fallback with original PCM and speaker ceiling");
    stage=0;assert(ac101s_audio_start(&audio,100,24)==ESP_OK);
    fail_sink_start=true;fail_sink_stop=2;
    assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_USB)==ESP_FAIL);
    assert(audio.output==AC101S_OUTPUT_USB&&audio.started&&audio.external_started&&!audio.external_ready&&!pa);
    assert(ac101s_audio_write(&audio,&frame,4)==ESP_ERR_INVALID_STATE);
    fail_sink_start=false;assert(ac101s_audio_stop(&audio)==ESP_OK&&!audio.started);
    assert(ac101s_audio_start(&audio,100,24)==ESP_OK);
    fail_sink_stop=1;
    assert(ac101s_audio_stop(&audio)==ESP_FAIL&&audio.started&&!audio.external_ready);
    assert(ac101s_audio_start(&audio,100,24)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_SPEAKER)==ESP_ERR_INVALID_STATE&&!audio.started);
    assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_SPEAKER)==ESP_OK);
    puts("PASS USB start/stop cleanup failure retains transport ownership, blocks writes and new sessions, and pending STOP cannot be revived by route changes");
    assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_USB)==ESP_OK);
    assert(ac101s_audio_start(&audio,100,24)==ESP_OK);
    peer_connected=false;publish_stop_during_cleanup=&audio;const int before_open=close_calls;
    assert(ac101s_audio_write(&audio,&frame,4)==ESP_ERR_INVALID_STATE);
    assert(!audio.started&&!audio.external_started&&!pa&&!codec_open&&close_calls==before_open);
    assert(atomic_load(&audio.stop_serial)!=atomic_load(&audio.stop_completed));
    assert(ac101s_audio_stop(&audio)==ESP_OK);peer_connected=true;
    assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_SPEAKER)==ESP_OK);
    puts("PASS USB removal while STOP is published during transport cleanup cannot reopen speaker or continue media PCM");
    return 0;
}
