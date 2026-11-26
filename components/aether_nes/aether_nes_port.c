#include "aether_nes_priv.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_timer.h"

#include <bitmap.h>
#include <event.h>
#include <gui.h>
#include <log.h>
#include <nes.h>
#include <nesinput.h>
#include <nofconfig.h>
#include <nofrendo.h>
#include <noftypes.h>
#include <osd.h>
#include <vid_drv.h>

#define OSD_SAMPLE_RATE        48000
#define OSD_AUDIO_FRAGMENT     400
#define OSD_TOUCH_POINTS       2

static const char *TAG = "aether_nes_port";
static aether_nes_handle_t s_active;
static void (*s_audio_callback)(void *buffer, int length);
static esp_timer_handle_t s_timer;
static uint32_t s_timer_period_us;
static bool s_timer_paused, s_manual_paused;
typedef void (*nes_tick_func_t)(void);
static _Atomic(nes_tick_func_t) s_tick_func;
static unsigned s_old_input_mask;
static bool s_consume_touch, s_consume_external;
static int64_t s_touch_due_us, s_exit_touch_us;
static int64_t s_input_due_us;
static int64_t s_metrics_us;
static uint32_t s_metrics_emulated;
static uint32_t s_metrics_rendered;
static int64_t s_frame_started_us;
static bool s_frame_started;
static struct { uint64_t core, apu, audio, convert, callback, input, yield; } s_cost;
static bitmap_t *s_screen_bitmap;
static uint8_t s_dummy_pixel;
static char s_config_path[64] = "/sdcard/ROMs/nofrendo.cfg";

static int driver_init(int width, int height);
static void driver_shutdown(void);
static int driver_set_mode(int width, int height);
static void driver_set_palette(rgb_t *palette);
static void driver_clear(uint8 color);
static bitmap_t *driver_lock_write(void);
static void driver_free_write(int num_dirties, rect_t *dirty_rects);
static void driver_blit(bitmap_t *bitmap, int num_dirties,
                        rect_t *dirty_rects);

static viddriver_t s_driver = {
    "Aether01 LVGL RGB565",
    driver_init,
    driver_shutdown,
    driver_set_mode,
    driver_set_palette,
    driver_clear,
    driver_lock_write,
    driver_free_write,
    driver_blit,
    false,
};

void aether_nes_port_set_active(aether_nes_handle_t handle)
{
    s_old_input_mask = 0;
    s_consume_touch = true;
    s_consume_external = false;
    s_touch_due_us = 0; s_exit_touch_us = 0;
    s_manual_paused = false;
    s_input_due_us = 0;
    s_metrics_us = esp_timer_get_time();
    s_metrics_emulated = 0;
    s_metrics_rendered = 0;
    s_frame_started = false;
    memset(&s_cost,0,sizeof(s_cost));
    s_active = handle;
}

char *osd_getromdata(void)
{
    return s_active ? (char *)s_active->rom_data : NULL;
}

int osd_storage_available(void)
{
    return s_active && aether_media_fs_available(&s_active->media_lease);
}

void osd_persistence_error(const char *detail)
{
    if (!s_active) return;
    portENTER_CRITICAL(&s_active->lock);
    const bool first = s_active->persistence_error == ESP_OK;
    if (first) {
        s_active->persistence_error = ESP_FAIL;
        s_active->last_error = ESP_FAIL;
        strlcpy(s_active->detail, detail, sizeof(s_active->detail));
    }
    portEXIT_CRITICAL(&s_active->lock);
    if (first) ESP_LOGW(TAG, "NES persistence: %s", detail);
}

static inline uint16_t rgb888_to_565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

void osd_output_audio(void)
{
    if (!s_active || !s_audio_callback || atomic_load(&s_active->paused)) {
        return;
    }
    int16_t mono[OSD_AUDIO_FRAGMENT];
    int16_t stereo[OSD_AUDIO_FRAGMENT * 2];
    int samples_left = OSD_SAMPLE_RATE / NES_REFRESH_RATE;
    while (samples_left > 0 && !s_active->stop_requested) {
        const int count = samples_left > OSD_AUDIO_FRAGMENT
                              ? OSD_AUDIO_FRAGMENT : samples_left;
        /* APU counters must keep advancing when the codec is absent or has
         * failed too. NULL is not a silent-advance buffer in this core. */
        const int64_t process_begin = esp_timer_get_time();
        s_audio_callback(mono, count);
        s_cost.apu += (uint64_t)(esp_timer_get_time() - process_begin);
        if (s_active->audio_started) {
            for (int i = 0; i < count; ++i) {
                stereo[i * 2] = mono[i];
                stereo[i * 2 + 1] = mono[i];
            }
            const int64_t output_begin = esp_timer_get_time();
            const esp_err_t audio_error = ac101s_audio_write(s_active->audio,stereo,(size_t)count * 4U);
            s_cost.audio += (uint64_t)(esp_timer_get_time() - output_begin);
            if (audio_error != ESP_OK) {
                ESP_LOGW(TAG, "NES audio write failed; stopping codec output");
                const esp_err_t stopped = ac101s_audio_stop(s_active->audio);
                s_active->audio_started = stopped != ESP_OK;
                portENTER_CRITICAL(&s_active->lock);
                ++s_active->audio_errors;
                if (stopped != ESP_OK) s_active->last_error = stopped;
                portEXIT_CRITICAL(&s_active->lock);
                /* A failed stop still owns the codec. End this session so the
                 * worker can retry its normal teardown; never publish the
                 * owner as free or repeatedly feed a broken DMA channel. */
                if (stopped != ESP_OK) atomic_store(&s_active->stop_requested, true);
            }
        }
        samples_left -= count;
    }
}

void osd_emulated_frame(int drawn)
{
    if (!s_active) return;
    if (s_frame_started) s_cost.core += (uint64_t)(esp_timer_get_time() - s_frame_started_us);
    s_frame_started = false;
    portENTER_CRITICAL(&s_active->lock);
    ++s_active->emulated_frames;
    if (!drawn) ++s_active->skipped_frames;
    portEXIT_CRITICAL(&s_active->lock);
}

void osd_frame_begin(void)
{
    s_frame_started_us = esp_timer_get_time();
    s_frame_started = true;
}

void osd_frame_end(void)
{
    if (!s_active) return;
    /* Yield after publishing video/input, so the required idle opportunity
     * does not insert an extra tick before each visible frame is handed off. */
    const int64_t yield_begin = esp_timer_get_time();
    vTaskDelay(1);
    s_cost.yield += (uint64_t)(esp_timer_get_time() - yield_begin);
    const int64_t now = esp_timer_get_time();
    const int64_t elapsed = now - s_metrics_us;
    if (elapsed >= 1000000) {
        portENTER_CRITICAL(&s_active->lock);
        const uint32_t frames = s_active->emulated_frames - s_metrics_emulated;
        s_active->emulation_fps_x100 = (uint32_t)((uint64_t)(s_active->emulated_frames - s_metrics_emulated) * 100000000U / (uint64_t)elapsed);
        s_active->render_fps_x100 = (uint32_t)((uint64_t)(s_active->frame_number - s_metrics_rendered) * 100000000U / (uint64_t)elapsed);
        if (frames) {
            s_active->core_us_per_frame = (uint32_t)(s_cost.core / frames);
            s_active->apu_us_per_frame = (uint32_t)(s_cost.apu / frames);
            s_active->audio_output_us_per_frame = (uint32_t)(s_cost.audio / frames);
            s_active->video_convert_us_per_frame = (uint32_t)(s_cost.convert / frames);
            s_active->frame_callback_us_per_frame = (uint32_t)(s_cost.callback / frames);
            s_active->input_us_per_frame = (uint32_t)(s_cost.input / frames);
            s_active->yield_us_per_frame = (uint32_t)(s_cost.yield / frames);
        }
        s_metrics_emulated = s_active->emulated_frames;
        s_metrics_rendered = s_active->frame_number;
        portEXIT_CRITICAL(&s_active->lock);
        s_metrics_us = now;
        memset(&s_cost,0,sizeof(s_cost));
    }

}

void osd_waitframe(void)
{
    osd_getinput();
    const TickType_t pause_poll = pdMS_TO_TICKS(20);
    vTaskDelay(s_active && atomic_load(&s_active->paused) && pause_poll ? pause_poll : 1);
}

void osd_setsound(void (*playfunc)(void *buffer, int length))
{
    s_audio_callback = playfunc;
}

void osd_getsoundinfo(sndinfo_t *info)
{
    info->sample_rate = OSD_SAMPLE_RATE;
    info->bps = 16;
}

static void timer_callback(void *argument)
{
    (void)argument;
    nes_tick_func_t tick = atomic_load(&s_tick_func);
    if (tick) {
        tick();
    }
}

int osd_installtimer(int frequency, void *func, int funcsize, void *counter,
                     int countersize)
{
    (void)funcsize;
    (void)counter;
    (void)countersize;
    if (frequency <= 0 || !func) {
        return -1;
    }
    /* Disable publication before timer operations. A failed delete retains the
     * handle for this next install; it must never become an orphan timer. */
    atomic_store(&s_tick_func, NULL);
    if (s_timer) {
        const esp_err_t stopped = esp_timer_stop(s_timer);
        if (stopped != ESP_OK && stopped != ESP_ERR_INVALID_STATE) return -1;
    } else {
        const esp_timer_create_args_t args = {.callback = timer_callback, .name = "nes_tick"};
        if (esp_timer_create(&args, &s_timer) != ESP_OK) return -1;
    }
    atomic_store(&s_tick_func, (nes_tick_func_t)func);
    s_timer_period_us = 1000000U / (uint32_t)frequency;
    s_timer_paused = false;
    const esp_err_t started = esp_timer_start_periodic(s_timer, s_timer_period_us);
    if (started != ESP_OK) atomic_store(&s_tick_func, NULL);
    return started == ESP_OK ? 0 : -1;
}

void osd_getvideoinfo(vidinfo_t *info)
{
    info->default_width = NES_SCREEN_WIDTH;
    info->default_height = NES_VISIBLE_HEIGHT;
    info->driver = &s_driver;
}

static int driver_init(int width, int height)
{
    (void)width;
    (void)height;
    return s_active && s_active->frame565 ? 0 : -1;
}

static void driver_shutdown(void)
{
    if (s_screen_bitmap) {
        bmp_destroy(&s_screen_bitmap);
    }
}

static int driver_set_mode(int width, int height)
{
    (void)width;
    (void)height;
    return 0;
}

static void driver_set_palette(rgb_t *palette)
{
    if (!s_active) {
        return;
    }
    for (int i = 0; i < 256; ++i) {
        s_active->palette565[i] = rgb888_to_565(palette[i].r, palette[i].g,
                                                palette[i].b);
    }
}

static void driver_clear(uint8 color)
{
    (void)color;
}

static bitmap_t *driver_lock_write(void)
{
    if (!s_screen_bitmap) {
        s_screen_bitmap = bmp_createhw(&s_dummy_pixel, NES_SCREEN_WIDTH,
                                       NES_VISIBLE_HEIGHT, 1);
    }
    return s_screen_bitmap;
}

static void driver_free_write(int num_dirties, rect_t *dirty_rects)
{
    (void)num_dirties;
    (void)dirty_rects;
}

static void driver_blit(bitmap_t *bitmap, int num_dirties,
                        rect_t *dirty_rects)
{
    (void)num_dirties;
    (void)dirty_rects;
    if (!s_active || !bitmap || !s_active->frame565 ||
        bitmap->width < (int)AETHER_NES_FRAME_WIDTH ||
        bitmap->height < (int)AETHER_NES_FRAME_HEIGHT || atomic_load(&s_active->paused)) return;
    const int64_t convert_begin = esp_timer_get_time();
    /* Nearest-neighbor 3:2 expansion keeps the original aspect ratio. The
     * 384x336 rectangle takes 10.75ms of raw QSPI at 48MHz, leaving time to
     * render and submit it within the panel's roughly 16.7ms TE interval. */
    _Static_assert(AETHER_NES_OUTPUT_WIDTH * 2 == AETHER_NES_FRAME_WIDTH * 3, "NES output scale");
    unsigned previous_y = UINT_MAX;
    for (unsigned y = 0; y < AETHER_NES_OUTPUT_HEIGHT; ++y) {
        const unsigned source_y = y * AETHER_NES_FRAME_HEIGHT / AETHER_NES_OUTPUT_HEIGHT;
        uint16_t *dst = s_active->frame565 + y * AETHER_NES_OUTPUT_WIDTH;
        if (source_y == previous_y) memcpy(dst, dst - AETHER_NES_OUTPUT_WIDTH,
                                           AETHER_NES_OUTPUT_WIDTH * sizeof(*dst));
        else {
            const uint8_t *src = bitmap->line[source_y];
            for (unsigned x = 0; x < AETHER_NES_FRAME_WIDTH; x += 2, dst += 3) {
                const uint16_t first = s_active->palette565[src[x]];
                dst[0] = first;
                dst[1] = first;
                dst[2] = s_active->palette565[src[x + 1]];
            }
        }
        previous_y = source_y;
    }
    s_cost.convert += (uint64_t)(esp_timer_get_time() - convert_begin);
    portENTER_CRITICAL(&s_active->lock);
    const uint32_t frame_number = ++s_active->frame_number;
    portEXIT_CRITICAL(&s_active->lock);
    if (s_active->frame_cb) {
        const int64_t callback_begin = esp_timer_get_time();
        s_active->frame_cb(s_active->frame565, AETHER_NES_OUTPUT_WIDTH,
                           AETHER_NES_OUTPUT_HEIGHT, frame_number,
                           s_active->user_ctx);
        s_cost.callback += (uint64_t)(esp_timer_get_time() - callback_begin);
    }
}

esp_err_t aether_nes_set_paused(aether_nes_handle_t handle, bool paused)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&handle->lock);
    if (!handle->running || atomic_load(&handle->stop_requested) ||
        (handle->state != AETHER_NES_STARTING && handle->state != AETHER_NES_RUNNING &&
         handle->state != AETHER_NES_PAUSED)) {
        portEXIT_CRITICAL(&handle->lock);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&handle->pause_request, paused ? 1 : 0);
    portEXIT_CRITICAL(&handle->lock);
    return ESP_OK;
}

esp_err_t aether_nes_set_screen_paused(aether_nes_handle_t handle, bool paused)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    atomic_store(&handle->screen_paused, paused);
    return ESP_OK;
}

/* Only the emulator task may call Nofrendo events or change its audio owner. */
static void apply_pause(bool paused)
{
    if (atomic_exchange(&s_active->paused, paused) == paused) return;
    if (paused && s_timer && s_timer_period_us) {
        const esp_err_t stopped = esp_timer_stop(s_timer);
        s_timer_paused = stopped == ESP_OK || stopped == ESP_ERR_INVALID_STATE;
    } else if (!paused && s_timer_paused) {
        const esp_err_t started = esp_timer_start_periodic(s_timer, s_timer_period_us);
        if (started != ESP_OK) {
            portENTER_CRITICAL(&s_active->lock);
            s_active->last_error = started;
            s_active->state = AETHER_NES_ERROR;
            strlcpy(s_active->detail, "NES 定时器恢复失败", sizeof(s_active->detail));
            portEXIT_CRITICAL(&s_active->lock);
            atomic_store(&s_active->stop_requested, true);
            main_quit();
            return;
        }
        s_timer_paused = false;
    }
    if (paused && s_active->audio_started) {
        const esp_err_t stopped = ac101s_audio_stop(s_active->audio);
        s_active->audio_started = stopped != ESP_OK;
        if (stopped != ESP_OK) {
            portENTER_CRITICAL(&s_active->lock);
            ++s_active->audio_errors; s_active->last_error = stopped;
            portEXIT_CRITICAL(&s_active->lock);
        }
    } else if (!paused && s_active->audio && !s_active->audio_started) {
        s_active->audio_started = ac101s_audio_start(s_active->audio, 65, 0.0f) == ESP_OK;
    }
    event_t handler = event_get(event_togglepause);
    if (handler) handler(INP_STATE_MAKE);
    portENTER_CRITICAL(&s_active->lock);
    s_active->state = paused ? AETHER_NES_PAUSED : AETHER_NES_RUNNING;
    portEXIT_CRITICAL(&s_active->lock);
    s_metrics_us = esp_timer_get_time();
    s_metrics_emulated = s_active->emulated_frames;
    s_metrics_rendered = s_active->frame_number;
    memset(&s_cost, 0, sizeof(s_cost));
}

void osd_getinput(void)
{
    static const struct {
        unsigned mask;
        int event;
    } events[] = {
        {1U << 0, event_joypad1_up},
        {1U << 1, event_joypad1_down},
        {1U << 2, event_joypad1_left},
        {1U << 3, event_joypad1_right},
        {1U << 4, event_joypad1_a},
        {1U << 5, event_joypad1_b},
        {1U << 6, event_joypad1_select},
        {1U << 7, event_joypad1_start},
    };
    if (!s_active) {
        return;
    }
    if (atomic_load(&s_active->stop_requested) ||
        !aether_media_fs_available(&s_active->media_lease)) {
        atomic_store(&s_active->stop_requested, true);
        main_quit();
        return;
    }
    const int pause_request = atomic_exchange(&s_active->pause_request, -1);
    if (pause_request >= 0) s_manual_paused = pause_request != 0;
    const bool screen_paused = atomic_load(&s_active->screen_paused);
    apply_pause(s_manual_paused || screen_paused);
    if (atomic_load(&s_active->stop_requested)) return;
    const int64_t now = esp_timer_get_time();
    if (now < s_input_due_us) return;
    s_input_due_us = now + 8000;
    unsigned mask = atomic_load(&s_active->external_input);
    const bool gated = screen_paused || (s_active->input_gate && s_active->input_gate(s_active->input_gate_context));
    if (!mask) s_consume_external = false;
    else if (gated) s_consume_external = true;
    if (gated || s_consume_external) mask = 0;
    /* The screen has no joypad hit boxes. Low-frequency raw touch only supports
     * holding the upper-left72x72 corner for1.2s to leave a game without HID. */
    if (screen_paused) {
        s_consume_touch = true;
        s_exit_touch_us = 0;
    } else if (now >= s_touch_due_us) {
        s_touch_due_us = now + 125000;
        esp_lcd_touch_point_data_t points[OSD_TOUCH_POINTS];
        uint8_t count = 0;
        const int64_t input_begin = esp_timer_get_time();
        esp_err_t input_error;
        if (s_active->touch_reader) {
            input_error = s_active->touch_reader(s_active->touch_reader_context, points, &count,
                                                 OSD_TOUCH_POINTS);
        } else {
            input_error = esp_lcd_touch_read_data(s_active->touch);
            if (input_error == ESP_OK)
                input_error = esp_lcd_touch_get_data(s_active->touch, points, &count, OSD_TOUCH_POINTS);
        }
        s_cost.input += (uint64_t)(esp_timer_get_time() - input_begin);
        if (input_error != ESP_OK) {
            s_consume_touch = true; s_exit_touch_us = 0;
            portENTER_CRITICAL(&s_active->lock); ++s_active->input_errors; portEXIT_CRITICAL(&s_active->lock);
        } else if (!count) { s_consume_touch = false; s_exit_touch_us = 0; }
        else if (gated || s_consume_touch) { s_consume_touch = true; s_exit_touch_us = 0; }
        else if (count == 1 && points[0].x < 72 && points[0].y < 72) {
            if (!s_exit_touch_us) s_exit_touch_us = now ? now : 1;
            if (now - s_exit_touch_us >= 1200000) mask |= AETHER_NES_INPUT_EXIT;
        } else s_exit_touch_us = 0;
    }
    if ((mask & AETHER_NES_INPUT_EXIT) && !(s_old_input_mask & AETHER_NES_INPUT_EXIT))
        atomic_store(&s_active->stop_requested, true);
    if (s_active->stop_requested) {
        main_quit();
        return;
    }
    const unsigned changed = mask ^ s_old_input_mask;
    s_old_input_mask = mask;
    /* All emulator-global state stays on its owner task. Touch buttons are
     * edge-triggered so holding a finger cannot repeatedly pause or reset. */
    if ((changed & mask) & (1U << 8)) {
        s_manual_paused = !s_manual_paused;
        apply_pause(s_manual_paused);
    }
    if ((changed & mask) & (1U << 9)) {
        s_manual_paused = false;
        apply_pause(false);
        if (atomic_load(&s_active->stop_requested)) return;
        event_t reset = event_get(event_hard_reset);
        if (reset) reset(INP_STATE_MAKE);
        portENTER_CRITICAL(&s_active->lock);
        s_active->state = AETHER_NES_RUNNING;
        portEXIT_CRITICAL(&s_active->lock);
    }
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        if (changed & events[i].mask) {
            event_t handler = event_get(events[i].event);
            if (handler) {
                handler((mask & events[i].mask) ? INP_STATE_MAKE
                                                : INP_STATE_BREAK);
            }
        }
    }
}

void osd_getmouse(int *x, int *y, int *button)
{
    (void)x;
    (void)y;
    (void)button;
}

static int nofrendo_log(const char *message)
{
    ESP_LOGD(TAG, "%s", message);
    return 0;
}

int osd_init(void)
{
    /* log_print dispatches to this hook; using log_print itself recurses in
     * builds with NOFRENDO_DEBUG enabled. */
    log_chain_logfunc(nofrendo_log);
    return 0;
}

void osd_shutdown(void)
{
    s_audio_callback = NULL;
}

static void select_config_path(void)
{
    const char *root = s_active && !strncmp(s_active->path, "/usb/", 5) ? "/usb" : "/sdcard";
    struct stat directory;
    char folder[32];
    snprintf(folder, sizeof(folder), "%s/ROMs", root);
    /* ROMs may also be launched from the root or another browser directory.
     * Keep legacy configuration when its folder exists; do not depend on a
     * ROMs directory just to close an otherwise valid external-disk session. */
    snprintf(s_config_path, sizeof(s_config_path), "%s%s/nofrendo.cfg", root,
             osd_storage_available() && !stat(folder, &directory) && S_ISDIR(directory.st_mode) ? "/ROMs" : "");
}

int osd_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    select_config_path();
    config.filename = s_config_path;
    return main_loop(s_active ? s_active->path : "rom.nes", system_nes);
}

void osd_fullname(char *fullname, const char *shortname)
{
    strlcpy(fullname, shortname, PATH_MAX);
}

char *osd_newextension(char *string, char *extension)
{
    char *slash = strrchr(string, '/');
    char *dot = strrchr(string, '.');
    if (!dot || (slash && dot < slash)) {
        dot = string + strlen(string);
    }
    strlcpy(dot, extension, PATH_MAX - (size_t)(dot - string));
    return string;
}

int osd_makesnapname(char *filename, int length)
{
    (void)filename;
    (void)length;
    return -1;
}

int aether_nes_port_run(void)
{
    char *argv[] = {"aether_nes", NULL};
    return nofrendo_main(1, argv);
}

bool aether_nes_port_mapper_supported(unsigned mapper_number)
{
    return mapper_number <= 255U && mmc_peek((int)mapper_number);
}

esp_err_t aether_nes_port_shutdown(void)
{
    esp_err_t error = ESP_OK;
    atomic_store(&s_tick_func, NULL);
    if (s_timer) {
        const esp_err_t stopped = esp_timer_stop(s_timer);
        if (stopped == ESP_OK || stopped == ESP_ERR_INVALID_STATE) {
            error = esp_timer_delete(s_timer);
            if (error == ESP_OK) s_timer = NULL;
            /* A failed delete remains owned and must be retried before play. */
        } else error = stopped;
    }
    s_audio_callback = NULL;
    s_active = NULL;
    s_timer_paused = false;
    s_timer_period_us = 0;
    return error;
}
