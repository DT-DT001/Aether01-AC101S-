/* Production microphone ownership/configuration and exact-length DMA reads.
 * Model failed codec setup and stop while testing the shared capture API. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../components/ac101s_audio/ac101s_audio.c"

struct test_mutex { bool locked; };
static struct ac101s_audio_dev_t *audio;
static TickType_t ticks, lock_delay;
static int pa, stage, fail_stage, open_calls, close_calls, programmed_gain;
static bool codec_open, mute, fail_close;
static unsigned sink_starts, sink_stops, sink_writes;
static bool capture_setup;
static bool usb_mic_connected=true;
static unsigned usb_capture_starts,usb_capture_stops,usb_capture_reads,usb_stop_fail;
static bool usb_source_connected(void*c){assert(c==(void*)4);return usb_mic_connected;}
static esp_err_t usb_source_start(void*c){assert(c==(void*)4&&!pa&&!codec_open);++usb_capture_starts;return ESP_OK;}
static esp_err_t usb_source_stop(void*c){assert(c==(void*)4&&!pa);++usb_capture_stops;if(usb_stop_fail){--usb_stop_fail;return ESP_FAIL;}return ESP_OK;}
static esp_err_t usb_source_read(void*c,void*p,size_t n,uint32_t timeout)
{assert(c==(void*)4&&p&&n==4096&&timeout==800&&!pa&&!codec_open);++usb_capture_reads;for(size_t i=0;i<n/2;++i)((int16_t*)p)[i]=(int16_t)(i%2?1234:-1234);return ESP_OK;}
static int event(void) { return ++stage == fail_stage ? ESP_FAIL : ESP_OK; }
TickType_t xTaskGetTickCount(void) { return ticks; }
int64_t esp_timer_get_time(void) { return (int64_t)ticks * 1000; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t timeout)
{
    if (h->locked) { ticks += timeout; return pdFALSE; }
    ticks += lock_delay; lock_delay = 0; h->locked = true; return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { assert(h->locked); h->locked=false; return pdTRUE; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    assert(pin == 42);
    if (level) assert(codec_open && !mute && !audio->capture_active && !capture_setup);
    pa = level;
    return capture_setup ? event() : ESP_OK;
}
esp_err_t gpio_input_enable(gpio_num_t pin) { assert(pin==42); return ESP_OK; }
esp_err_t gpio_set_direction(gpio_num_t pin, int dir) { assert(pin==42 && dir==GPIO_MODE_OUTPUT); return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { assert(pin==42); return pa; }
esp_err_t ac101s_codec_start(i2c_master_dev_handle_t dev, bool capture_only, float gain_db)
{
    assert(dev && !pa && gain_db >= 0 && gain_db <= 42);
    codec_open = mute = true;
    ++open_calls;
    programmed_gain = (int)gain_db;
    return capture_only && capture_setup ? event() : ESP_OK;
}

esp_err_t ac101s_codec_stop(i2c_master_dev_handle_t dev)
{
    assert(dev && !pa);
    ++close_calls;
    if (fail_close) { fail_close = false; return ESP_FAIL; }
    codec_open = false;
    return ESP_OK;
}

esp_err_t ac101s_codec_set_speaker(i2c_master_dev_handle_t dev, unsigned level)
{
    assert(dev && !pa && level <= 70);
    mute = level == 0;
    return ESP_OK;
}

static bool connected(void *context) { assert(context==(void*)3); return true; }
static esp_err_t transport_start(void *context) { assert(context==(void*)3 && !pa); ++sink_starts; return ESP_OK; }
static esp_err_t transport_stop(void *context) { assert(context==(void*)3 && !pa); ++sink_stops; return ESP_OK; }
static esp_err_t transport_write(void *c, const void *p, size_t n, uint32_t timeout)
{ assert(c==(void*)3 && p && n && timeout); ++sink_writes; return ESP_OK; }

struct read_step { esp_err_t error; size_t bytes; unsigned elapsed, expected_timeout; };
static struct read_step reads[8];
static unsigned read_count, read_index;
static size_t fill_offset;
static unsigned board_writes;
static void read_plan(const struct read_step *steps, unsigned n)
{ memcpy(reads,steps,n*sizeof(*steps)); read_count=n; read_index=0; fill_offset=0; }
int i2s_channel_read(i2s_chan_handle_t rx, void *data, size_t bytes, size_t *received, unsigned timeout)
{
    assert(rx==(void*)2 && data && bytes && !pa && mute && codec_open && audio->capture_ready);
    assert(read_index<read_count && timeout==reads[read_index].expected_timeout);
    const struct read_step *s=&reads[read_index++]; ticks+=s->elapsed; *received=s->bytes;
    for(size_t i=0;i<s->bytes && i<bytes;++i) ((uint8_t*)data)[i]=(uint8_t)(fill_offset+i);
    fill_offset+=s->bytes; return s->error;
}
int i2s_channel_write(i2s_chan_handle_t tx, const void *data, size_t bytes, size_t *sent, unsigned timeout)
{
    assert(tx==(void*)5 && data && bytes && timeout && codec_open);
    if (audio->board_owner) {
        assert(bytes <= 960 && timeout <= 30);
        ++board_writes;
    }
    *sent=bytes;
    return ESP_OK;
}
static void reset_setup(void)
{
    assert(!audio->started && !audio->capture_active && !codec_open && !pa);
    capture_setup=true; fail_stage=0; stage=0;
}

int main(void)
{
    struct test_mutex mutex={0};
    struct ac101s_audio_dev_t dev={.codec=(void*)1,.rx=(void*)2,.tx=(void*)5,.pa_enable=42,
        .sample_rate_hz=48000,.master_volume=100,.mutex=&mutex,.route_lock=portMUX_INITIALIZER_UNLOCKED};
    audio=&dev;
    const ac101s_audio_sink_t sink={.context=(void*)3,.connected=connected,.start=transport_start,
        .stop=transport_stop,.write=transport_write};
    assert(ac101s_audio_set_external_sink(audio,&sink)==ESP_OK);
    assert(ac101s_audio_set_output(audio,AC101S_OUTPUT_EXTERNAL)==ESP_OK);
    uint8_t pcm[4096];
    for (unsigned i=0;i<100;++i) {
        reset_setup();
        assert(ac101s_audio_capture_start(audio,24)==ESP_OK);
        assert(stage==2 && codec_open && mute && !pa && audio->capture_active && audio->capture_ready);
        assert(!audio->started && !audio->external_started && programmed_gain==24);
        assert(audio->output==AC101S_OUTPUT_EXTERNAL && !sink_starts && !sink_stops);
        capture_setup=false;
        assert(ac101s_audio_capture_start(audio,24)==ESP_ERR_INVALID_STATE);
        assert(ac101s_audio_start(audio,70,24)==ESP_ERR_INVALID_STATE);
        assert(ac101s_audio_write(audio,pcm,sizeof pcm)==ESP_ERR_INVALID_STATE);
        assert(ac101s_audio_read(audio,pcm,sizeof pcm)==ESP_ERR_INVALID_STATE);
        assert(ac101s_audio_set_output(audio,AC101S_OUTPUT_EXTERNAL)==ESP_OK);
        assert(ac101s_audio_set_output(audio,AC101S_OUTPUT_SPEAKER)==ESP_ERR_INVALID_STATE);
        assert(ac101s_audio_set_external_sink(audio,NULL)==ESP_ERR_INVALID_STATE);
        assert(ac101s_audio_set_master_volume(audio,0)==ESP_OK && !pa && mute);
        assert(ac101s_audio_set_master_volume(audio,100)==ESP_OK && !pa && mute);
        assert(ac101s_audio_stop(audio)==ESP_OK && audio->capture_ready && codec_open && !pa);
        const struct read_step good[]={{ESP_OK,sizeof pcm,22,1000}};
        read_plan(good,1); assert(ac101s_audio_capture_read(audio,pcm,sizeof pcm)==ESP_OK);
        for(size_t k=0;k<sizeof pcm;++k) assert(pcm[k]==(uint8_t)(k-(k%4>=2?2:0)));
        assert(ac101s_audio_capture_stop(audio)==ESP_OK && !audio->capture_active && !audio->capture_ready && !codec_open && !pa);
        assert(ac101s_audio_capture_stop(audio)==ESP_OK);
    }
    assert(open_calls==100 && close_calls==100);
    puts("PASS 100 local microphone sessions while external route selected: ADC slots, DAC mute, PA off, playback exclusion, volume, stale generic STOP");

    for (int fault=1;fault<=2;++fault) {
        reset_setup(); fail_stage=fault;
        assert(ac101s_audio_capture_start(audio,24)==ESP_FAIL);
        assert(!audio->capture_active && !audio->capture_ready && !codec_open && !pa && !mutex.locked);
        capture_setup=false; assert(ac101s_audio_capture_stop(audio)==ESP_OK);
    }
    reset_setup();
    assert(ac101s_audio_capture_start(audio,23.9f)==ESP_OK && programmed_gain==23);
    capture_setup=false;
    assert(ac101s_audio_capture_stop(audio)==ESP_OK);
    puts("PASS capture PA/codec setup failures roll back and sub-dB gain stays bounded");

    reset_setup(); assert(ac101s_audio_capture_start(audio,24)==ESP_OK); capture_setup=false;
    const struct read_step short_ok[]={{ESP_OK,1024,100,800},{ESP_OK,3072,15,700}};
    lock_delay=200; read_plan(short_ok,2);
    assert(ac101s_audio_capture_read(audio,pcm,sizeof pcm)==ESP_OK && read_index==2);
    for(size_t k=0;k<sizeof pcm;++k) assert(pcm[k]==(uint8_t)(k-(k%4>=2?2:0)));
    const struct read_step partial_timeout[]={{ESP_OK,1024,300,1000},{ESP_ERR_TIMEOUT,512,700,700}};
    read_plan(partial_timeout,2); TickType_t began=ticks;
    assert(ac101s_audio_capture_read(audio,pcm,sizeof pcm)==ESP_ERR_TIMEOUT && ticks-began==1000);
    const struct read_step short_empty[]={{ESP_OK,0,0,1000}};
    read_plan(short_empty,1); assert(ac101s_audio_capture_read(audio,pcm,sizeof pcm)==ESP_FAIL);
    const struct read_step corrupt_length[]={{ESP_OK,2,1,1000}};
    read_plan(corrupt_length,1); assert(ac101s_audio_capture_read(audio,pcm,sizeof pcm)==ESP_FAIL);
    const struct read_step expired[]={{ESP_OK,4,1000,1000}};
    read_plan(expired,1); assert(ac101s_audio_capture_read(audio,pcm,sizeof pcm)==ESP_ERR_TIMEOUT && read_index==1);
    read_count=read_index=0; lock_delay=1000;
    assert(ac101s_audio_capture_read(audio,pcm,sizeof pcm)==ESP_ERR_TIMEOUT && !read_index);
    mutex.locked=true; began=ticks;
    assert(ac101s_audio_capture_read(audio,pcm,sizeof pcm)==ESP_ERR_TIMEOUT && ticks-began==1000); mutex.locked=false;
    assert(ac101s_audio_capture_read(audio,pcm,3)==ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_capture_read(audio,NULL,4)==ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_capture_read(audio,pcm,(size_t)INT_MAX+1)==ESP_ERR_INVALID_ARG);
    puts("PASS exact-length PCM assembly, lock+DMA shared 1s budget, partial timeout, empty/misaligned read and buffer validation");

    fail_close=true;
    assert(ac101s_audio_capture_stop(audio)==ESP_FAIL && audio->capture_active && !audio->capture_ready && !pa);
    assert(ac101s_audio_capture_read(audio,pcm,sizeof pcm)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_start(audio,70,24)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_capture_start(audio,24)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_set_output(audio,AC101S_OUTPUT_SPEAKER)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_capture_stop(audio)==ESP_OK && !audio->capture_active && !codec_open);
    assert(ac101s_audio_set_output(audio,AC101S_OUTPUT_SPEAKER)==ESP_OK);
    assert(ac101s_audio_start(audio,70,24)==ESP_OK && pa && !mute);
    const unsigned closed=close_calls;
    assert(ac101s_audio_capture_stop(audio)==ESP_OK && close_calls==(int)closed && audio->started && pa && !mute);
    assert(ac101s_audio_capture_start(audio,24)==ESP_ERR_INVALID_STATE && audio->started && pa);
    assert(ac101s_audio_write(audio,pcm,sizeof pcm)==ESP_OK);
    assert(ac101s_audio_stop(audio)==ESP_OK);
    assert(ac101s_audio_set_output(audio,AC101S_OUTPUT_EXTERNAL)==ESP_OK);
    assert(ac101s_audio_start(audio,70,24)==ESP_OK && sink_starts==1);
    assert(ac101s_audio_capture_stop(audio)==ESP_OK && !sink_stops && audio->external_started);
    assert(ac101s_audio_stop(audio)==ESP_OK && sink_stops==1);
    assert(ac101s_audio_capture_start(NULL,24)==ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_capture_start(audio,NAN)==ESP_ERR_INVALID_ARG);
    assert(ac101s_audio_capture_start(audio,43)==ESP_ERR_INVALID_ARG);
    audio->sample_rate_hz=44100;
    assert(ac101s_audio_capture_start(audio,24)==ESP_ERR_NOT_SUPPORTED);
    puts("PASS failed-close ownership retained for retry; stale capture STOP leaves newer speaker/external playback untouched; invalid gain/rate rejected");
    audio->sample_rate_hz=48000;capture_setup=false;
    const ac101s_audio_source_t usb={.context=(void*)4,.connected=usb_source_connected,
        .start=usb_source_start,.stop=usb_source_stop,.read=usb_source_read};
    assert(ac101s_audio_set_usb_source(audio,&usb)==ESP_OK);
    assert(ac101s_audio_set_input(audio,AC101S_INPUT_USB)==ESP_OK);
    assert(ac101s_audio_capture_start(audio,24)==ESP_OK&&usb_capture_starts==1&&!codec_open&&!pa);
    assert(ac101s_audio_set_input(audio,AC101S_INPUT_BOARD)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_set_usb_source(audio,NULL)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_start(audio,100,24)==ESP_ERR_INVALID_STATE);
    lock_delay=200;
    assert(ac101s_audio_capture_read(audio,pcm,sizeof pcm)==ESP_OK&&usb_capture_reads==1);
    for(size_t i=0;i<sizeof pcm/2;++i)assert(((int16_t*)pcm)[i]==(i%2?1234:-1234));
    usb_mic_connected=false;
    assert(ac101s_audio_capture_read(audio,pcm,sizeof pcm)==ESP_ERR_INVALID_STATE&&usb_capture_reads==1);
    usb_stop_fail=1;
    assert(ac101s_audio_capture_stop(audio)==ESP_FAIL&&audio->capture_active&&!audio->capture_ready);
    assert(ac101s_audio_set_input(audio,AC101S_INPUT_BOARD)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_capture_stop(audio)==ESP_OK&&!audio->capture_active&&usb_capture_stops==2);
    assert(ac101s_audio_set_input(audio,AC101S_INPUT_BOARD)==ESP_OK);
    ac101s_audio_input_t selected;assert(ac101s_audio_get_input(audio,&selected)==ESP_OK&&selected==AC101S_INPUT_BOARD);
    assert(ac101s_audio_set_input(audio,AC101S_INPUT_USB)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_set_output(audio,AC101S_OUTPUT_SPEAKER)==ESP_OK);
    assert(ac101s_audio_start(audio,100,24)==ESP_OK&&pa);
    assert(ac101s_audio_capture_stop(audio)==ESP_OK&&pa&&usb_capture_stops==2);
    assert(ac101s_audio_stop(audio)==ESP_OK);
    puts("PASS USB mic packed48kstereo exact PCM and remaining-budget transport; playback exclusion, removal returns failure, failed-stop source ownership retained, board input recovery and stale capture-stop safety");

    /* Intercom reserves only the physical codec, preserving the user's USB
     * input and external output selections for the next normal session. */
    static int intercom, another_client;
    usb_mic_connected=true;
    assert(ac101s_audio_set_input(audio,AC101S_INPUT_USB)==ESP_OK);
    assert(ac101s_audio_set_output(audio,AC101S_OUTPUT_EXTERNAL)==ESP_OK);
    reset_setup();
    assert(ac101s_audio_board_start(audio,&intercom,true,100,24)==ESP_OK);
    capture_setup=false;
    assert(codec_open && mute && !pa && usb_capture_starts==1);
    assert(ac101s_audio_capture_read(audio,pcm,960)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_capture_stop(audio)==ESP_OK && codec_open);
    assert(ac101s_audio_board_pcm(audio,&another_client,pcm,960,60)==ESP_ERR_INVALID_STATE);
    const struct read_step bounded[]={{ESP_OK,960,4,30},{ESP_OK,960,4,28}};
    read_plan(bounded,2);
    assert(ac101s_audio_board_pcm(audio,&intercom,pcm,1920,60)==ESP_OK && read_index==2);
    assert(ac101s_audio_board_stop(audio,&intercom)==ESP_OK && !codec_open);
    assert(ac101s_audio_board_start(audio,&intercom,false,100,24)==ESP_OK && pa);
    assert(ac101s_audio_stop(audio)==ESP_OK && pa);
    assert(ac101s_audio_board_stop(audio,&another_client)==ESP_OK && pa);
    assert(ac101s_audio_write(audio,pcm,960)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_set_output(audio,AC101S_OUTPUT_SPEAKER)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_board_pcm(audio,&intercom,pcm,1920,60)==ESP_OK && board_writes==2);
    fail_close=true;
    assert(ac101s_audio_board_stop(audio,&intercom)==ESP_FAIL && !pa && codec_open);
    assert(ac101s_audio_set_master_volume(audio,85)==ESP_OK && !pa);
    assert(ac101s_audio_board_stop(audio,&intercom)==ESP_OK && !codec_open);
    assert(ac101s_audio_get_input(audio,&selected)==ESP_OK && selected==AC101S_INPUT_USB);
    ac101s_audio_output_t selected_output;
    assert(ac101s_audio_get_output(audio,&selected_output)==ESP_OK && selected_output==AC101S_OUTPUT_EXTERNAL);
    assert(ac101s_audio_start(audio,100,24)==ESP_OK && sink_starts==2 && !pa);
    assert(ac101s_audio_board_start(audio,&intercom,false,100,24)==ESP_ERR_INVALID_STATE);
    assert(ac101s_audio_board_stop(audio,&intercom)==ESP_OK && sink_stops==1);
    assert(ac101s_audio_stop(audio)==ESP_OK && sink_stops==2);
    puts("PASS exclusive intercom mic/speaker, normal route and stop isolation, bounded DMA and transient codec close recovery");
    return 0;
}
