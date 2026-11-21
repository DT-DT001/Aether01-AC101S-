/* Real platform adapter; only hardware/RTOS operations are substituted. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "stubs/runtime.h"
static bool config_folder_exists;
static int config_directory_stat(const char *name,struct stat *info)
{assert(!strcmp(name,"/sdcard/ROMs")||!strcmp(name,"/usb/ROMs"));if(!config_folder_exists)return -1;memset(info,0,sizeof(*info));info->st_mode=S_IFDIR;return 0;}
#define stat(...) config_directory_stat(__VA_ARGS__)
#include "../../components/aether_nes/aether_nes_port.c"
#undef stat

static struct aether_nes_t handle;
static uint16_t frame[AETHER_NES_OUTPUT_WIDTH * AETHER_NES_OUTPUT_HEIGHT];
static int64_t clock_us;
static unsigned generated, write_calls, written_bytes, stop_calls, start_calls;
static unsigned touch_reads, delay_ticks, quit_calls, frame_callbacks;
static unsigned event_calls[event_last][2];
static int current_event;
static bool card_inserted, write_failure, gate_consumes;
static unsigned gate_calls;
static unsigned process_us, output_us, callback_us, input_us, yield_us;
static esp_err_t read_error, get_error;
static esp_err_t audio_stop_error;
static uint8_t point_count;
static esp_lcd_touch_point_data_t touch_points[2];
size_t strlcpy(char *d,const char *s,size_t n)
{size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l;}

int64_t esp_timer_get_time(void) { return clock_us; }
void vTaskDelay(uint32_t ticks) { delay_ticks += ticks; clock_us += (int64_t)ticks*yield_us; }
esp_err_t aether_nes_get_status(aether_nes_handle_t h,aether_nes_status_t *status)
{assert(h==&handle);memset(status,0,sizeof(*status));return ESP_OK;}
bool aether_storage_card_inserted(aether_storage_handle_t storage)
{ assert(storage == handle.storage); return card_inserted; }
esp_err_t ac101s_audio_write(ac101s_audio_handle_t audio, void *data, size_t bytes)
{
    assert(audio == handle.audio);
    assert(bytes > 0 && bytes % 4 == 0);
    const int16_t *samples = data;
    for (size_t i = 0; i < bytes / 4; ++i) assert(samples[i * 2] == samples[i * 2 + 1]);
    ++write_calls;
    clock_us += output_us;
    if (write_failure) return ESP_FAIL;
    written_bytes += (unsigned)bytes;
    return ESP_OK;
}
esp_err_t ac101s_audio_start(ac101s_audio_handle_t audio, int volume, float gain)
{ assert(audio == handle.audio); (void)volume; (void)gain; ++start_calls; return ESP_OK; }
esp_err_t ac101s_audio_stop(ac101s_audio_handle_t audio)
{ assert(audio == handle.audio); ++stop_calls; return audio_stop_error; }
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t touch)
{ assert(touch == handle.touch); ++touch_reads; clock_us += input_us; return read_error; }
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t touch,
    esp_lcd_touch_point_data_t *points, uint8_t *count, uint8_t maximum)
{
    assert(touch == handle.touch && maximum >= point_count);
    if (get_error != ESP_OK) return get_error;
    *count = point_count;
    memcpy(points, touch_points, point_count * sizeof(*points));
    return ESP_OK;
}
static void record_event(int code)
{ assert(code == INP_STATE_MAKE || code == INP_STATE_BREAK); ++event_calls[current_event][code == INP_STATE_MAKE]; }
event_t event_get(int event) { assert(event > 0 && event < event_last); current_event = event; return record_event; }
void main_quit(void) { ++quit_calls; }
static bool input_gate(void *context)
{ assert(context == &handle); ++gate_calls; return gate_consumes; }
static void generate_audio(void *buffer, int samples)
{
    int16_t *output = buffer;
    assert(buffer && samples > 0);
    for (int i = 0; i < samples; ++i) output[i] = (int16_t)((generated + i) % 30000U - 15000);
    generated += (unsigned)samples;
    clock_us += process_us;
}
static void on_frame(const uint16_t *pixels, uint16_t width, uint16_t height,
                     uint32_t number, void *context)
{
    assert(pixels == frame && width == 480 && height == 420 && number > 0);
    assert(context == &handle);
    ++frame_callbacks;
    clock_us += callback_us;
}
static void reset(void)
{
    memset(&handle, 0, sizeof(handle));
    handle.media_lease=(aether_media_fs_lease_t){.tf=(void*)1,.held=true};
    handle.storage = (void *)1;
    handle.audio = (void *)2;
    handle.touch = (void *)3;
    handle.frame565 = frame;
    handle.audio_started = true;
    handle.frame_cb = on_frame;
    handle.user_ctx = &handle;
    handle.input_gate = input_gate;
    handle.input_gate_context = &handle;
    atomic_init(&handle.stop_requested, false);
    atomic_init(&handle.paused, false);
    atomic_init(&handle.pause_request, -1);
    atomic_init(&handle.external_input, 0);
    handle.running = true;
    handle.state = AETHER_NES_RUNNING;
    clock_us = 0;
    generated = write_calls = written_bytes = stop_calls = start_calls = 0;
    touch_reads = delay_ticks = quit_calls = frame_callbacks = gate_calls = 0;
    memset(event_calls, 0, sizeof(event_calls));
    card_inserted = true;
    write_failure = gate_consumes = false;
    read_error = get_error = ESP_OK;
    audio_stop_error = ESP_OK;
    process_us = output_us = callback_us = input_us = yield_us = 0;
    point_count = 0;
    aether_nes_port_set_active(&handle);
    osd_setsound(generate_audio);
}
static void poll_at(int64_t now, uint16_t x, uint16_t y, bool pressed)
{
    clock_us = now;
    point_count = pressed;
    touch_points[0] = (esp_lcd_touch_point_data_t){.x = x, .y = y};
    osd_getinput();
}
static void test_audio_and_measurement(void)
{
    reset();
    for (unsigned i = 0; i < 60; ++i) {
        clock_us = (int64_t)(i + 1) * 1000000 / 60;
        if (!(i % 3)) ++handle.frame_number;
        osd_emulated_frame(!(i % 3));
        osd_frame_end();
    }
    assert(generated == 48000 && written_bytes == 48000 * 4);
    assert(handle.emulated_frames == 60 && handle.skipped_frames == 40);
    assert(handle.emulation_fps_x100 == 6000 && handle.render_fps_x100 == 2000);
    assert(delay_ticks >= 60); /* Sustained backlog also gives idle a chance. */
    reset();
    handle.audio_started = false;
    osd_emulated_frame(0);
    assert(generated == 800 && !write_calls); /* APU progresses without codec. */
    reset();
    write_failure = true;
    osd_emulated_frame(1);
    osd_emulated_frame(0);
    assert(generated == 1600 && write_calls == 1 && stop_calls == 1);
    assert(handle.audio_errors == 1 && !handle.audio_started);
    atomic_store(&handle.paused, true);
    write_audio_frame();
    assert(generated == 1600); /* Pausing freezes APU time. */
    atomic_store(&handle.paused, false);
    atomic_store(&handle.stop_requested, true);
    write_audio_frame();
    assert(generated == 1600); /* Stop never fills another I2S fragment. */
    reset();
    write_failure = true;
    audio_stop_error = ESP_ERR_TIMEOUT;
    osd_emulated_frame(1);
    assert(generated == 400 && write_calls == 1 && stop_calls == 1);
    assert(handle.audio_started && handle.audio_errors == 1 &&
           handle.last_error == ESP_ERR_TIMEOUT && atomic_load(&handle.stop_requested));
    write_audio_frame();
    assert(generated == 400 && write_calls == 1 && stop_calls == 1);
}
static void test_stage_measurement(void)
{
    reset();
    process_us=10; output_us=20; callback_us=30; input_us=40; yield_us=1000;
    bitmap_t *bitmap=bmp_create(256,224,0); assert(bitmap);
    memset(bitmap->data,0,224U*(size_t)bitmap->pitch);
    for(unsigned i=0;i<50;++i) {
        osd_frame_begin(); clock_us+=12000;
        osd_emulated_frame(1); driver_blit(bitmap,0,NULL); osd_getinput();
        clock_us=(int64_t)(i+1)*20000-1000; osd_frame_end();
    }
    assert(handle.emulation_fps_x100==5000 && handle.render_fps_x100==5000);
    assert(handle.core_us_per_frame==12000 && handle.apu_us_per_frame==20);
    assert(handle.audio_output_us_per_frame==40 && handle.frame_callback_us_per_frame==30);
    assert(handle.input_us_per_frame>0 && handle.input_us_per_frame<=40 && handle.yield_us_per_frame==1000);
    assert(handle.video_convert_us_per_frame==0); /* Virtual clock does not advance in conversion. */
    bmp_destroy(&bitmap);
}
static void test_blit_has_no_audio_clock(void)
{
    reset();
    bitmap_t *bitmap = bmp_create(256, 224, 0);
    assert(bitmap);
    for (int i = 0; i < 256; ++i) handle.palette565[i] = (uint16_t)(i * 17);
    for (int y=0;y<224;++y)for(int x=0;x<256;++x)bitmap->line[y][x]=(uint8_t)(x+3*y);
    driver_blit(bitmap, 0, NULL);
    assert(frame_callbacks == 1 && generated == 0 && write_calls == 0);
    for (unsigned y=0;y<420;++y) for (unsigned x=0;x<480;++x)
        assert(frame[y*480+x] == (uint8_t)(x*256/480+3*(y*224/420))*17);
    atomic_store(&handle.paused, true);
    driver_blit(bitmap, 0, NULL);
    assert(frame_callbacks == 1);
    atomic_store(&handle.paused, false);
    bitmap->width = 255;
    driver_blit(bitmap, 0, NULL);
    assert(frame_callbacks == 1); /* Reject short lines before reading pixels. */
    bmp_destroy(&bitmap);
}
static void test_external_controls_and_touch_exit(void)
{
    reset();
    /* No invisible joypad: arbitrary screen coordinates never emit keys. */
    poll_at(0,415,350,true);
    assert(!event_calls[event_joypad1_a][1]);
    atomic_store(&handle.external_input,AETHER_NES_INPUT_UP|AETHER_NES_INPUT_RIGHT|AETHER_NES_INPUT_A);
    clock_us=8000;osd_getinput();
    assert(event_calls[event_joypad1_up][1]==1 && event_calls[event_joypad1_right][1]==1 && event_calls[event_joypad1_a][1]==1);
    atomic_store(&handle.external_input,0);clock_us=16000;osd_getinput();
    assert(event_calls[event_joypad1_up][0]==1 && event_calls[event_joypad1_a][0]==1);
    gate_consumes=true;atomic_store(&handle.external_input,AETHER_NES_INPUT_A);clock_us=24000;osd_getinput();
    gate_consumes=false;clock_us=32000;osd_getinput();
    assert(event_calls[event_joypad1_a][1]==1); /* Held sleep gesture stays consumed. */
    atomic_store(&handle.external_input,0);clock_us=40000;osd_getinput();
    atomic_store(&handle.external_input,AETHER_NES_INPUT_PAUSE);clock_us=48000;osd_getinput();
    assert(atomic_load(&handle.paused)&&stop_calls==1);
    clock_us=56000;osd_getinput();assert(stop_calls==1);
    atomic_store(&handle.external_input,0);clock_us=64000;osd_getinput();
    atomic_store(&handle.external_input,AETHER_NES_INPUT_RESET);clock_us=72000;osd_getinput();
    assert(!atomic_load(&handle.paused)&&start_calls==1&&event_calls[event_hard_reset][1]==1);
    atomic_store(&handle.external_input,AETHER_NES_INPUT_EXIT);clock_us=80000;osd_getinput();
    assert(quit_calls==1&&atomic_load(&handle.stop_requested));
    reset();poll_at(0,0,0,false);poll_at(125000,20,20,true);
    for(unsigned n=2;n<=10;++n)poll_at(n*125000,20,20,true);
    assert(!quit_calls);
    poll_at(1375000,20,20,true);assert(quit_calls==1);
    reset();poll_at(0,0,0,false);read_error=ESP_FAIL;poll_at(125000,20,20,true);
    read_error=ESP_OK;poll_at(250000,20,20,true);poll_at(1800000,20,20,true);assert(!quit_calls);
    poll_at(1925000,0,0,false);poll_at(2050000,20,20,true);poll_at(3300000,20,20,true);assert(quit_calls==1);
    reset();read_error=ESP_FAIL;poll_at(0,0,0,false);assert(handle.input_errors==1);
    card_inserted=false;clock_us=1000;osd_getinput();assert(quit_calls==1&&touch_reads==1);
}

static unsigned timer_creates,timer_deletes,ticks;
static bool timer_live,timer_running;
static esp_err_t timer_stop_error,timer_delete_error;
static void (*fake_timer_callback)(void *);
esp_err_t esp_timer_create(const esp_timer_create_args_t *a,esp_timer_handle_t *h)
{ assert(!timer_live);++timer_creates;timer_live=true;fake_timer_callback=a->callback;*h=(void*)1;return ESP_OK; }
esp_err_t esp_timer_start_periodic(esp_timer_handle_t h,uint64_t us)
{assert(h==(void*)1&&timer_live&&us==16666);timer_running=true;return ESP_OK;}
esp_err_t esp_timer_stop(esp_timer_handle_t h)
{assert(h==(void*)1&&timer_live);if(timer_stop_error)return timer_stop_error;timer_running=false;return ESP_OK;}
esp_err_t esp_timer_delete(esp_timer_handle_t h)
{assert(h==(void*)1&&timer_live&&!timer_running);if(timer_delete_error)return timer_delete_error;timer_live=false;++timer_deletes;return ESP_OK;}
static void tick(void){++ticks;}
static void test_timer_cleanup(void)
{
    reset();assert(osd_installtimer(60,(void*)tick,0,NULL,0)==0&&timer_live);
    fake_timer_callback(NULL);assert(ticks==1);
    timer_delete_error=ESP_FAIL;assert(aether_nes_port_shutdown()==ESP_FAIL&&timer_live&&!timer_running);
    fake_timer_callback(NULL);assert(ticks==1&&!s_tick_func&&!s_active);
    timer_delete_error=ESP_OK;assert(aether_nes_port_shutdown()==ESP_OK&&!timer_live);
    assert(osd_installtimer(60,(void*)tick,0,NULL,0)==0);timer_stop_error=ESP_FAIL;
    assert(aether_nes_port_shutdown()==ESP_FAIL&&timer_running);fake_timer_callback(NULL);assert(ticks==1);
    assert(osd_installtimer(60,(void*)tick,0,NULL,0)==-1&&timer_creates==2);
    timer_stop_error=ESP_OK;assert(aether_nes_port_shutdown()==ESP_OK&&timer_deletes==2);
    for(unsigned i=0;i<100;++i){assert(osd_installtimer(60,(void*)tick,0,NULL,0)==0);assert(aether_nes_port_shutdown()==ESP_OK);}
    assert(!timer_live&&timer_creates==timer_deletes);
}

static void test_remote_pause_owner(void)
{
    reset();
    assert(aether_nes_set_paused(NULL, true) == ESP_ERR_INVALID_ARG);
    assert(aether_nes_set_paused(&handle, true) == ESP_OK);
    assert(!atomic_load(&handle.paused) && !stop_calls);
    osd_getinput();
    assert(atomic_load(&handle.paused) && stop_calls == 1);
    assert(handle.state == AETHER_NES_PAUSED && event_calls[event_togglepause][1] == 1);
    assert(aether_nes_set_paused(&handle, true) == ESP_OK);
    osd_getinput(); /* Request processing bypasses the I2C poll limiter. */
    assert(stop_calls == 1 && event_calls[event_togglepause][1] == 1);
    read_error = ESP_FAIL;
    poll_at(20000, 0, 0, false);
    assert(aether_nes_set_paused(&handle, false) == ESP_OK);
    osd_getinput(); /* A failed touch bus must not block a remote resume. */
    assert(!atomic_load(&handle.paused) && start_calls == 1);
    assert(handle.state == AETHER_NES_RUNNING && event_calls[event_togglepause][1] == 2);
    assert(aether_nes_set_paused(&handle, false) == ESP_OK);
    osd_getinput();
    assert(start_calls == 1);
    handle.running = false;
    assert(aether_nes_set_paused(&handle, true) == ESP_ERR_INVALID_STATE);

    reset();
    handle.state = AETHER_NES_STARTING;
    assert(aether_nes_set_paused(&handle, true) == ESP_OK);
    handle.state = AETHER_NES_RUNNING;
    osd_getinput();
    assert(atomic_load(&handle.paused) && stop_calls == 1);
    assert(aether_nes_set_paused(&handle, false) == ESP_OK);
    card_inserted = false;
    osd_getinput();
    assert(quit_calls == 1 && !start_calls); /* Unplug wins over queued resume. */
    assert(aether_nes_set_paused(&handle, false) == ESP_ERR_INVALID_STATE);

    reset();
    assert(aether_nes_set_paused(&handle, true) == ESP_OK);
    atomic_store(&handle.stop_requested, true);
    osd_getinput();
    assert(quit_calls == 1 && !stop_calls && !event_calls[event_togglepause][1]);
}

int main(void)
{
    test_audio_and_measurement();test_stage_measurement();test_blit_has_no_audio_clock();
    test_external_controls_and_touch_exit();test_remote_pause_owner();test_timer_cleanup();
    reset();assert(osd_storage_available());card_inserted=false;assert(!osd_storage_available());
    osd_persistence_error("old save retained");osd_persistence_error("later error");
    assert(handle.persistence_error==ESP_FAIL&&handle.last_error==ESP_FAIL&&!strcmp(handle.detail,"old save retained"));
    s_active=NULL;assert(!osd_storage_available());osd_persistence_error("no session");
    reset();config_folder_exists=false;select_config_path();assert(!strcmp(s_config_path,"/sdcard/nofrendo.cfg"));
    config_folder_exists=true;select_config_path();assert(!strcmp(s_config_path,"/sdcard/ROMs/nofrendo.cfg"));
    strcpy(handle.path,"/usb/Games/game.nes");select_config_path();assert(!strcmp(s_config_path,"/usb/ROMs/nofrendo.cfg"));
    config_folder_exists=false;select_config_path();assert(!strcmp(s_config_path,"/usb/nofrendo.cfg"));
    puts("PASS NES producer exact480x420 nearest pixels, two audio writes/frame, HID edges/disconnect/sleep gate, low-frequency touch exit and100 timer cleanup cycles/faults");
    return 0;
}
