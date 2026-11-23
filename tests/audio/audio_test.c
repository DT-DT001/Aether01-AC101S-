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
static int actual_volume;
static bool fail_close;
static bool check_local_pcm;
static uint32_t local_pcm;
static TickType_t ticks, lock_ticks, local_write_ticks;
static int local_write_error;
static bool checking_alert, short_alert_write;
static unsigned alert_writes;
static size_t alert_bytes;
static bool capture_local;
static size_t short_write;
static int16_t captured[4096];
static size_t captured_samples;
esp_err_t gpio_input_enable(gpio_num_t pin) { assert(pin == 42); return ESP_OK; }
esp_err_t gpio_set_direction(gpio_num_t pin, int mode) { assert(pin == 42 && mode == GPIO_MODE_OUTPUT); return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { assert(pin == 42); return pa; }
TickType_t xTaskGetTickCount(void) { return ticks; }
int64_t esp_timer_get_time(void) { return (int64_t)ticks * 1000; }
void vTaskDelay(TickType_t duration) { ticks += duration; }

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
    if (fail_stage == stage) return ESP_FAIL;
    actual_volume = (int)volume;
    muted = volume == 0;
    return ESP_OK;
}

static unsigned clock_enables, clock_disables;
int i2s_channel_enable(i2s_chan_handle_t channel) { assert(channel); ++clock_enables; return ESP_OK; }
int i2s_channel_disable(i2s_chan_handle_t channel) { assert(channel); ++clock_disables; return ESP_OK; }
int i2s_channel_write(i2s_chan_handle_t tx, const void *data, size_t size, size_t *sent, unsigned timeout)
{
    assert(tx && timeout <= 1000);
    if (checking_alert) {
        assert(size && size <= 240 * 4 && !(size % 4) && timeout <= 100 && pa);
        ++alert_writes;
        alert_bytes += size;
    } else assert(size == 4 || (capture_local && size <= AUDIO_DMA_FRAMES * PCM16_STEREO_BYTES));
    ticks += local_write_ticks;
    if (check_local_pcm) memcpy(&local_pcm, data, 4);
    *sent = checking_alert && short_alert_write ? 0 : size;
    if (capture_local) {
        if (short_write) { *sent = short_write; short_write = 0; }
        assert(captured_samples + *sent / 2 <= sizeof(captured) / sizeof(*captured));
        memcpy(captured + captured_samples, data, *sent);
        captured_samples += *sent / 2;
    }
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

static void write_tone(ac101s_audio_handle_t audio, size_t frames)
{
    int16_t pcm[2880];
    assert(frames * 2 <= sizeof(pcm) / sizeof(*pcm));
    for (size_t i = 0; i < frames * 2; i += 2) { pcm[i] = 30000; pcm[i + 1] = -30000; }
    captured_samples = 0;
    assert(ac101s_audio_write(audio, pcm, frames * PCM16_STEREO_BYTES) == ESP_OK);
    assert(captured_samples == frames * 2);
    for (size_t i = 0; i < frames * 2; i += 2) {
        assert(pcm[i] == 30000 && pcm[i + 1] == -30000);
        assert(captured[i] == -captured[i + 1]);
    }
}

int main(void)
{
    struct test_mutex mutex = {0};
    struct ac101s_audio_dev_t audio = {.codec = (void *)1, .tx = (void *)2, .rx = (void *)3, .pa_enable = 42,
                                     .sample_rate_hz = 48000, .mutex = &mutex, .master_volume = 100,
                                     .speaker_ring_frames = 6 * AUDIO_DMA_FRAMES,
                                     .route_lock = portMUX_INITIALIZER_UNLOCKED};
    uint32_t frame = 0;
    ac101s_audio_write_metrics_t initial_metrics;
    assert(ac101s_audio_get_write_metrics(&audio,&initial_metrics)==ESP_OK);
    assert(!initial_metrics.write_calls && !initial_metrics.mutex_wait_max_us && !initial_metrics.output_write_max_us);
    /* Repeated sessions explicitly clear the persisted mute and gate PA. */
    for (unsigned session = 0; session < 100; ++session) {
        stage = 0;
        assert(ac101s_audio_start(&audio, 70, 24.0f) == ESP_OK);
        assert(actual_volume == 70);
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

    /* The PCM follows while codec/PA and the shared I/O mutex stay untouched. */
    stage=0; assert(ac101s_audio_start(&audio,100,24)==ESP_OK);
    capture_local=true;
    mutex.locked=true;
    const TickType_t before_volume=ticks;
    assert(ac101s_audio_set_master_volume(&audio,50)==ESP_OK);
    uint8_t master;
    assert(ac101s_audio_get_master_volume(&audio,&master)==ESP_OK && master==50 && ticks==before_volume);
    mutex.locked=false;
    short_write=12;
    write_tone(&audio, PLAYBACK_GAIN_RAMP_FRAMES);
    assert(captured[0] > 29900 && captured[captured_samples-2]==5334);
    for(size_t i=2;i<captured_samples;i+=2) {
        assert(captured[i]<=captured[i-2] && captured[i-2]-captured[i]<=50);
    }
    assert(ac101s_audio_set_master_volume(&audio,0)==ESP_OK);
    write_tone(&audio, PLAYBACK_GAIN_RAMP_FRAMES);
    assert(captured[0]>5300 && !captured[captured_samples-2] && pa && stage==2);
    write_tone(&audio, audio.speaker_ring_frames - 1);
    assert(pa);
    for(size_t i=0;i<captured_samples;++i) assert(!captured[i]);
    write_tone(&audio, 1);
    assert(!pa && stage==2 && actual_volume==70);
    assert(ac101s_audio_stop(&audio)==ESP_OK);
    stage=0; assert(ac101s_audio_start(&audio,100,24)==ESP_OK && !pa);
    write_tone(&audio, 8);
    assert(!captured[0] && !pa);
    assert(ac101s_audio_set_master_volume(&audio,100)==ESP_OK);
    write_tone(&audio, 100);
    const int previous=captured[captured_samples-2];
    assert(previous>5000 && previous<6000 && pa);
    assert(ac101s_audio_set_master_volume(&audio,2)==ESP_OK);
    write_tone(&audio, PLAYBACK_GAIN_RAMP_FRAMES);
    assert(captured[0]<=previous && previous-captured[0]<16 && captured[captured_samples-2]==268);
    /* These settled amplitudes preserve the old -27, -15 and -9 dB anchors
     * relative to 100%. Between anchors every volume has a finer PCM level. */
    static const struct { uint8_t level; int amplitude; } anchors[] = {
        {10, 1340}, {50, 5334}, {70, 10644}, {100, 30000},
    };
    for (size_t i=0;i<sizeof(anchors)/sizeof(*anchors);++i) {
        assert(ac101s_audio_set_master_volume(&audio,anchors[i].level)==ESP_OK);
        write_tone(&audio,PLAYBACK_GAIN_RAMP_FRAMES);
        assert(captured[captured_samples-2]==anchors[i].amplitude);
    }
    assert(stage==2 && actual_volume==70 && pa);
    capture_local=false;
    assert(ac101s_audio_stop(&audio)==ESP_OK);
    puts("PASS lock-free master preview, stereo 512-frame ramp across partial writes, retargeting continuity, speaker 10/50/70/100 anchors and digital zero/DMA drain");

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
    for(unsigned i=0;i<1500;++i) {
        const int gain=(int32_t)original[i]*1225/10000;
        if(i>=2*PLAYBACK_GAIN_RAMP_FRAMES) assert(abs(captured[i]-gain)<=1);
        else assert(abs(captured[i])+1>=abs(gain));
    }
    captured_samples=0; assert(ac101s_audio_set_master_volume(&audio,0)==ESP_OK);
    assert(ac101s_audio_write(&audio,pcm,sizeof(pcm))==ESP_OK);
    assert(captured[0]<-3600);
    for(unsigned i=2*PLAYBACK_GAIN_RAMP_FRAMES;i<1500;++i) assert(!captured[i]);
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
            assert(actual_volume==(stream ? 70 : 0));
            assert((bool)pa==(stream>0)&&muted==(stream==0) && stage==2);
            uint8_t logical=255;assert(ac101s_audio_get_master_volume(&audio,&logical)==ESP_OK&&logical==master);
        }
        assert(ac101s_audio_stop(&audio)==ESP_OK&&!pa&&muted);
    }
    puts("PASS 10,201 stream/master combinations: speaker ceiling programmed once, logical values exact, preview has no codec or PA side effects");

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
        write_tone(&audio,PLAYBACK_GAIN_RAMP_FRAMES);
        captured_samples=0;
        assert(ac101s_audio_write(&audio,pcm,4)==ESP_OK&&captured[0]==-8192&&captured[1]==8191);
        assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_EXTERNAL)==ESP_OK&&!pa);
        assert(ac101s_audio_write(&audio,pcm,4)==ESP_OK);
        assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_USB)==ESP_OK);
        /* Removal applies local gain once to the original source PCM. */
        peer_connected=false;check_local_pcm=true;
        assert(ac101s_audio_write(&audio,pcm,4)==ESP_OK&&audio.started&&!audio.external_started);
        assert(audio.output==AC101S_OUTPUT_SPEAKER&&local_pcm==0x16c2e93dU&&actual_volume==70&&pa);
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

    const int16_t chirp[480 * 2] = {250, -250};
    assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_USB)==ESP_OK);
    const unsigned previous_starts=sink_starts, previous_stops=sink_stops;
    const int previous_volume=audio.requested_volume;
    const float previous_gain=audio.mic_gain_db;
    checking_alert=true;
    stage=0;
    assert(ac101s_audio_play_alert(&audio,chirp,480)==ESP_OK);
    assert(alert_writes==2&&alert_bytes==sizeof(chirp)&&actual_volume==60);
    assert(!pa&&!codec_open&&!audio.started&&!audio.board_owner&&!mutex.locked);
    assert(audio.output==AC101S_OUTPUT_USB&&sink_starts==previous_starts&&sink_stops==previous_stops);
    assert(audio.requested_volume==previous_volume&&audio.mic_gain_db==previous_gain&&audio.master_volume==100);
    assert(ac101s_audio_play_alert(&audio,chirp,0)==ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_play_alert(&audio,chirp,5761)==ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_start(&audio,100,24)==ESP_OK);
    assert(ac101s_audio_play_alert(&audio,chirp,480)==ESP_ERR_INVALID_STATE&&audio.external_started);
    assert(ac101s_audio_stop(&audio)==ESP_OK);
    audio.capture_active=true;
    assert(ac101s_audio_play_alert(&audio,chirp,480)==ESP_ERR_INVALID_STATE&&audio.capture_active);
    audio.capture_active=false;
    stage=0;
    const int intercom=1;
    assert(ac101s_audio_board_start(&audio,&intercom,false,65,24)==ESP_OK);
    assert(ac101s_audio_play_alert(&audio,chirp,480)==ESP_ERR_INVALID_STATE&&audio.board_owner==&intercom&&pa);
    assert(ac101s_audio_board_stop(&audio,&intercom)==ESP_OK&&alert_writes==2);
    mutex.locked=true;
    assert(ac101s_audio_play_alert(&audio,chirp,480)==ESP_ERR_INVALID_STATE&&mutex.locked);
    mutex.locked=false;
    stage=0;short_alert_write=true;
    assert(ac101s_audio_play_alert(&audio,chirp,480)==ESP_FAIL&&!pa&&!codec_open&&!audio.started);
    short_alert_write=false;stage=0;fail_close=true;
    assert(ac101s_audio_play_alert(&audio,chirp,480)==ESP_FAIL&&!pa&&!audio.local_ready);
    assert(audio.started&&audio.alert_cleanup_pending&&!audio.board_owner&&audio.output==AC101S_OUTPUT_USB);
    const unsigned after_alert=sink_starts;
    assert(ac101s_audio_start(&audio,100,24)==ESP_FAIL&&sink_starts==after_alert);
    fail_close=false;
    assert(ac101s_audio_start(&audio,100,24)==ESP_OK&&sink_starts==after_alert+1);
    assert(!audio.alert_cleanup_pending&&audio.external_started&&ac101s_audio_stop(&audio)==ESP_OK);
    assert(ac101s_audio_set_master_volume(&audio,0)==ESP_OK);
    const unsigned before_muted=alert_writes;
    assert(ac101s_audio_play_alert(&audio,chirp,480)==ESP_OK&&alert_writes==before_muted&&!codec_open);
    puts("PASS alert chunks remain bounded, yield to playback/capture/intercom, preserve USB route, respect mute, and recover a retained codec");
    const unsigned enables = clock_enables;
    assert(ac101s_audio_idle_clocks(&audio)==ESP_OK && clock_disables==2);
    assert(ac101s_audio_idle_clocks(&audio)==ESP_OK && clock_disables==2);
    assert(ac101s_audio_set_output(&audio,AC101S_OUTPUT_SPEAKER)==ESP_OK);
    stage=0;
    assert(ac101s_audio_start(&audio,100,24)==ESP_OK && clock_enables==enables+2);
    assert(ac101s_audio_idle_clocks(&audio)==ESP_ERR_INVALID_STATE && clock_disables==2);
    assert(ac101s_audio_stop(&audio)==ESP_OK);
    assert(ac101s_audio_idle_clocks(&audio)==ESP_OK && clock_disables==4);
    puts("PASS idle I2S releases clocks, rejects active audio, and wakes with both clocks on");
    return 0;
}
