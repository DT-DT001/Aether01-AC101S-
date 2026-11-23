#include "aether_audio_player.h"
#include "aether_recorder.h"
#include "aether_board.h"
#include "aether_video_player.h"
#include "aether_nes.h"
#include "aether_storage.h"
#include "aether_usb_msc.h"
#include "app_power.h"
#include "app_otg.h"
#include "app_gps.h"
#ifdef AETHER_GPS_DIAGNOSTIC
#include "app_gps_diagnostic.h"
#endif
#include "aether_usb_audio.h"
#include "app_usb_input.h"
#include "app_telemetry.h"
#include "app_touch.h"
#include "app_display_metrics.h"
#include "app_display_sync.h"
#include "app_tools.h"
#include "app_pinyin_extended.h"
#include "app_ui.h"
#include "app_audio_system.h"
#include "app_nes_controls.h"
#include "app_pc_remote.h"
#include "app_displacement.h"
#include "app_intercom.h"
#include "app_utilities.h"
#include "app_media_control.h"
#ifdef AETHER_VIDEO_AUTOPROBE
#include "app_video_probe.h"
#endif
#ifdef AETHER_RADIO_SELFTEST
#include "app_radio_diagnostic.h"
#endif
#ifdef AETHER_UI_SELFTEST
#include "app_ui_selftest.h"
#endif
#ifdef AETHER_AUDIO_DIAGNOSTIC
#include "app_audio_diagnostic.h"
#endif
#include "aether_wifi.h"
#include "ui/app_home.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "esp_psram.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#ifdef AETHER_CODEC_SELFTEST
#include "aether_codec_check.h"
#endif

static const char *TAG = "aether01";
static aether_board_t s_board;
static aether_storage_handle_t s_storage;
static aether_audio_player_handle_t s_audio_player;
static aether_recorder_handle_t s_recorder;
static aether_video_player_handle_t s_video_player;
static aether_nes_handle_t s_nes;
static aether_wifi_handle_t s_wifi;
static aether_bluetooth_handle_t s_bluetooth;
static app_power_t s_power;
static app_ui_t *s_ui;

#if defined(AETHER_VIDEO_AUTOPROBE) && defined(AETHER_RADIO_SELFTEST)
static void video_probe_radio_followup(void *unused)
{
    (void)unused;
    /* This optional controller only enqueues commands and reads cached states;
     * actual radio initialization and NVS stay in the internal owner tasks. */
    static unsigned pass;
    app_radio_diagnostic_run(app_audio_system_bluetooth(), s_wifi, pass++ == 0);
}
#endif

#if defined(AETHER_VIDEO_DIAGNOSTIC) || defined(AETHER_VIDEO_AUTOPROBE)
static void video_diagnostic_probe(void *context)
{
    aether_video_player_handle_t player = context;
    static bool was_active;
    aether_video_status_t s = {0};
    if (!player || aether_video_player_get_status(player, &s) != ESP_OK) return;
    const bool active = s.state == AETHER_VIDEO_STARTING || s.state == AETHER_VIDEO_PLAYING || s.state == AETHER_VIDEO_PAUSED;
    if (!active && !was_active) return;
    was_active = active;
    ESP_LOGI("video_diag", "state=%u pos=%lu clock=%lu seek=%u target=%lu frames=%lu fps100=%lu drop=%lu late=%lu lookup=%lu/%lu read=%lu decode=%lu cb=%lu scan=%lu preroll=%lu clock_wait=%lu err=%X",
        (unsigned)s.state,(unsigned long)s.position_ms,(unsigned long)s.elapsed_ms,s.seeking,(unsigned long)s.seek_requested_ms,
        (unsigned long)s.frame_number,(unsigned long)s.actual_fps_x100,(unsigned long)s.dropped_frames,
        (unsigned long)s.late_frames,(unsigned long)s.video_lookup_us,(unsigned long)s.video_lookup_max_us,
        (unsigned long)s.read_us,(unsigned long)s.decode_us,
        (unsigned long)s.callback_us,(unsigned long)s.seek_scan_us,(unsigned long)s.seek_preroll_us,
        (unsigned long)s.audio_clock_wait_us,(unsigned)s.last_error);
    ESP_LOGI("video_diag", "audio=%u err=%X lookup=%lu/%lu read=%lu/%lu decode=%lu/%lu write=%lu/%lu gap=%lu/%lu late=%lu",
        s.has_audio,(unsigned)s.audio_error,(unsigned long)s.audio_lookup_us,(unsigned long)s.audio_lookup_max_us,
        (unsigned long)s.audio_read_us,(unsigned long)s.audio_read_max_us,
        (unsigned long)s.audio_decode_us,(unsigned long)s.audio_decode_max_us,
        (unsigned long)s.audio_write_us,(unsigned long)s.audio_write_max_us,
        (unsigned long)s.audio_supply_gap_us,(unsigned long)s.audio_supply_gap_max_us,(unsigned long)s.audio_late_units);
    ESP_LOGI("video_diag", "stages h264=%lu/%lu convert=%lu/%lu read_max=%lu decode_max=%lu cb_max=%lu lag_resets=%lu skip_burst_max=%lu decoded=%lu converted=%lu presentation_drops=%lu h264_total=%llu convert_total=%llu",
        (unsigned long)s.h264_us,(unsigned long)s.h264_max_us,
        (unsigned long)s.convert_us,(unsigned long)s.convert_max_us,
        (unsigned long)s.read_max_us,(unsigned long)s.decode_max_us,(unsigned long)s.callback_max_us,
        (unsigned long)s.lag_rebuilds,(unsigned long)s.lag_skip_burst_max,
        (unsigned long)s.h264_output_frames,(unsigned long)s.converted_frames,(unsigned long)s.presentation_drops,
        (unsigned long long)s.h264_total_us,(unsigned long long)s.convert_total_us);
    ESP_LOGI("video_diag", "placement video_affinity=%ld video_cores=0x%lX audio_affinity=%ld audio_cores=0x%lX",
        (long)s.video_affinity,(unsigned long)s.video_core_mask,
        (long)s.audio_affinity,(unsigned long)s.audio_core_mask);
    ESP_LOGI("video_diag", "stacks video=0x%08lX hp=%u free_min=%lu audio=0x%08lX hp=%u free_min=%lu",
        (unsigned long)s.video_stack_addr,s.video_stack_hp,(unsigned long)s.video_stack_free_min_bytes,
        (unsigned long)s.audio_stack_addr,s.audio_stack_hp,(unsigned long)s.audio_stack_free_min_bytes);
    ac101s_audio_write_metrics_t audio_io;
    if (ac101s_audio_get_write_metrics(s_board.audio, &audio_io) == ESP_OK) {
        ESP_LOGI("video_diag", "output lock=%lu/%lu payload=%lu/%lu calls=%lu mutex_timeouts=%lu errors=%lu last=%X",
            (unsigned long)audio_io.mutex_wait_us,(unsigned long)audio_io.mutex_wait_max_us,
            (unsigned long)audio_io.output_write_us,(unsigned long)audio_io.output_write_max_us,
            (unsigned long)audio_io.write_calls,(unsigned long)audio_io.mutex_timeouts,
            (unsigned long)audio_io.output_errors,(unsigned)audio_io.last_error);
    }
}
#endif

static void remote_media_control(aether_bluetooth_remote_action_t action, void *context)
{
    (void)context;
    const esp_err_t error = app_ui_queue_media_control(s_ui, action);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "remote media command not queued: %s", esp_err_to_name(error));
}

static bool media_flash_busy(void *context)
{
    (void)context;
    if (s_recorder) {
        aether_recorder_status_t recorder;
        if (aether_recorder_get_status(s_recorder, &recorder) != ESP_OK || recorder.active) return true;
    }
    return app_media_control_flash_busy(s_audio_player, s_video_player, s_nes);
}

typedef struct {
    bool active, storage_gate, lease_gate, wifi_enabled, bluetooth_audio_requested, clock_pending;
    bool ui_resume_requested;
    uint32_t clock_revision;
} shutdown_context_t;
static shutdown_context_t s_shutdown;
static esp_err_t usb_vbus_present(bool *present,void *context) {
    aether_board_t *board=context;
    if(!board || !board->pmic || !present) return ESP_ERR_INVALID_ARG;
    tg29_battery_state_t battery;
    esp_err_t error=tg29_get_battery_state(board->pmic,&battery);
    if(error==ESP_OK && !battery.vbus_present) return ESP_ERR_INVALID_STATE; /* GOOD=false is not proof of physical absence. */
    if(error==ESP_OK) *present=true;
    return error;
}
static uint32_t shutdown_remaining(int64_t deadline) {
    int64_t left=deadline-esp_timer_get_time();
    return left>0 ? (uint32_t)(left/1000) : 0;
}
static esp_err_t prepare_shutdown(void *context,app_power_shutdown_stage_t stage,uint32_t timeout_ms) {
    shutdown_context_t *state=context;
    const int64_t deadline=esp_timer_get_time()+(int64_t)timeout_ms*1000;
    aether_bluetooth_handle_t bluetooth=s_bluetooth;
    if(stage==APP_POWER_SHUTDOWN_BEGIN) {
        *state=(shutdown_context_t){.active=true};
        app_gps_cancel();
        /* Stop source admission promptly; cleanup shares the existing budget. */
        (void)app_otg_set_enabled(false);
        app_pc_remote_leave();
        app_intercom_leave();
        app_displacement_cancel();
        aether_wifi_status_t wifi={0};aether_bluetooth_status_t bt={0};
        if(s_wifi && aether_wifi_get_status(s_wifi,&wifi)==ESP_OK) state->wifi_enabled=wifi.enabled_preference;
        if(bluetooth && aether_bluetooth_get_status(bluetooth,&bt)==ESP_OK) state->bluetooth_audio_requested=bt.audio_requested;
        /* UI/IO reserve the same command gate used by USB entry before closing
         * the current reader and requesting every media owner to stop. */
        app_tools_set_clock_quiesced(true);
        app_tools_snapshot_t tools;app_tools_get_snapshot(&tools);state->clock_pending=tools.clock_pending;state->clock_revision=tools.clock_revision;
        const esp_err_t ui_error=app_ui_quiesce_storage_begin(s_ui);
        app_intercom_leave();
        if(ui_error!=ESP_OK && ui_error!=ESP_ERR_TIMEOUT) return ui_error;
        esp_err_t error=aether_storage_gate_new_users(s_storage,true);
        if(error!=ESP_OK) return error;
        state->lease_gate=true;
        app_tools_get_snapshot(&tools);
        state->clock_pending=state->clock_pending || tools.clock_pending || tools.clock_revision!=state->clock_revision;
        app_audio_system_quiesce_controls(true);
        if(bluetooth) {error=aether_bluetooth_set_enabled(bluetooth,false);if(error!=ESP_OK) ESP_LOGW(TAG,"shutdown Bluetooth stop request: %s",esp_err_to_name(error));}
        if(s_wifi) {error=aether_wifi_set_enabled(s_wifi,false);if(error!=ESP_OK) ESP_LOGW(TAG,"shutdown Wi-Fi stop request: %s",esp_err_to_name(error));}
        return ui_error;
    }
    if(stage==APP_POWER_SHUTDOWN_FINALIZE) {
        esp_err_t error=app_gps_stop_wait(shutdown_remaining(deadline));
        if(error!=ESP_OK) return error;
        error=app_otg_disable_wait(shutdown_remaining(deadline));
        if(error!=ESP_OK) return error;
        error=app_ui_quiesce_storage_wait(s_ui,shutdown_remaining(deadline));
        if(error!=ESP_OK) return error;
        if(!shutdown_remaining(deadline)) return ESP_ERR_TIMEOUT;
        state->storage_gate=true;
        error=aether_storage_prepare_shutdown(s_storage,shutdown_remaining(deadline));
        if(error!=ESP_OK) return error;
        for(;;) {
            app_audio_system_snapshot_t audio={0};app_audio_system_get_snapshot(&audio);
            app_tools_snapshot_t tools={0};app_tools_get_snapshot(&tools);
            app_nes_controls_snapshot_t controls={0};app_nes_controls_get_snapshot(&controls);
            bool pending=controls.pending || app_pc_remote_save_pending() || app_intercom_busy() || app_utilities_save_pending() ||
                (audio.ready && !audio.controls_quiesced) || audio.persistence_pending || tools.preferences_pending || tools.clock_pending;
            if(state->clock_pending && !tools.clock_pending && tools.clock_error!=ESP_OK) return tools.clock_error;
            if(s_audio_player) {
                aether_audio_player_status_t music;
                error=aether_audio_player_get_status(s_audio_player,&music);
                if(error!=ESP_OK) return error;
                pending=pending || music.active || music.preferences_pending;
            }
            if(s_wifi) {
                aether_wifi_status_t wifi;error=aether_wifi_get_status(s_wifi,&wifi);if(error!=ESP_OK) return error;
                pending=pending || wifi.persistence_pending || wifi.enabled || wifi.state!=AETHER_WIFI_OFF;
            }
            if(bluetooth) {
                aether_bluetooth_status_t bt;error=aether_bluetooth_get_status(bluetooth,&bt);if(error!=ESP_OK) return error;
                pending=pending || bt.enabled || bt.state!=AETHER_BT_OFF;
            }
            if(!pending) {
                const esp_err_t utilities_error = app_utilities_save_error();
                if (utilities_error != ESP_OK)
                    ESP_LOGW(TAG, "utility preferences: %s", esp_err_to_name(utilities_error));
                break;
            }
            if(shutdown_remaining(deadline)<20) return ESP_ERR_TIMEOUT;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return ESP_OK;
    }
    if(stage==APP_POWER_SHUTDOWN_CANCEL) {
        if(!state->active) return ESP_OK;
        esp_err_t error=ESP_OK;
        if(state->storage_gate) {
            error=aether_storage_cancel_shutdown(s_storage,shutdown_remaining(deadline));
            if(error==ESP_OK) state->storage_gate=false;
        }
        if(!state->ui_resume_requested) {
            const esp_err_t resumed=app_ui_quiesce_storage_resume(s_ui);
            if(resumed==ESP_OK) state->ui_resume_requested=true;
            if(error==ESP_OK) error=resumed;
        }
        /* Resume is a request: retain the context and storage admission gate
         * until the IO worker and GUI have acknowledged it. Contention or a
         * delayed FILE operation is retried by the power task, never forgotten. */
        if(error==ESP_OK && app_ui_input_blocked(s_ui)) error=ESP_ERR_NOT_FINISHED;
        if(error!=ESP_OK) return error;
        if(state->lease_gate) {
            error=aether_storage_gate_new_users(s_storage,false);
            if(error!=ESP_OK) return error;
            state->lease_gate=false;
        }
        app_audio_system_quiesce_controls(false);
        app_tools_set_clock_quiesced(false);
        if(state->bluetooth_audio_requested && bluetooth) {
            esp_err_t result=aether_bluetooth_set_enabled(bluetooth,true);
            if(result!=ESP_OK) ESP_LOGW(TAG,"shutdown cancel Bluetooth enable: %s",esp_err_to_name(result));
        }
        if(state->wifi_enabled && s_wifi) {
            esp_err_t result=aether_wifi_set_enabled(s_wifi,true);
            if(result!=ESP_OK) ESP_LOGW(TAG,"shutdown cancel Wi-Fi enable: %s",esp_err_to_name(result));
        }
        state->active=false;
        return error;
    }
    return ESP_ERR_INVALID_ARG;
}

static bool nes_input_gate(void *context)
{
    app_power_t *power = context;
    /* Raw NES touch cannot wake an intentionally sleeping screen. */
    return app_power_is_sleeping(power);
}

static void intercom_user_key_press(void *context, int64_t pressed_at_us)
{
    if (!app_ui_input_blocked(context)) (void)app_intercom_user_key(pressed_at_us);
}

/* Generic GUI navigation stays isolated from gameplay HID shortcuts. */
static bool usb_ui_input_gate(void *context)
{
    app_power_status_t power;
    if (app_ui_input_blocked(s_ui) || app_power_get_status(&s_power, &power) != ESP_OK ||
        power.shutdown_recovering ||
        power.shutdown_state == APP_POWER_SHUTDOWN_PREPARING ||
        power.shutdown_state == APP_POWER_SHUTDOWN_REQUESTED) return true;
    aether_nes_status_t status;
    aether_nes_handle_t nes = context;
    return nes && aether_nes_get_status(nes, &status) == ESP_OK &&
        (status.state == AETHER_NES_STARTING || status.state == AETHER_NES_RUNNING ||
         status.state == AETHER_NES_PAUSED);
}

static void check_flash_capacity(void)
{
    uint32_t size = 0, id = 0;
    esp_err_t err = esp_flash_get_size(NULL, &size);
    if (err != ESP_OK) { ESP_LOGW(TAG, "flash capacity unavailable: %s", esp_err_to_name(err)); return; }
    esp_flash_read_id(NULL, &id);
    ESP_LOGI(TAG, "Flash JEDEC 0x%06lx, detected %lu MiB", (unsigned long)id,
             (unsigned long)(size / (1024U * 1024U)));
    if (size < 64U * 1024U * 1024U) ESP_LOGW(TAG, "expected 64 MiB flash");
    if (size > 16U * 1024U * 1024U) {
        uint8_t probe[16];
        err = esp_flash_read(NULL, probe, size - 4096U, sizeof(probe));
        ESP_LOGI(TAG, "read-only high-address flash probe @0x%08lx: %s",
                 (unsigned long)(size - 4096U), esp_err_to_name(err));
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase incompatible NVS");
        err = nvs_flash_init();
    }
    return err;
}

enum { BOOT_LOGO_WIDTH = 432, BOOT_LOGO_HEIGHT = 203, BOOT_LOGO_MIN_VISIBLE_MS = 2500 };
extern const uint8_t boot_logo_data[] asm("_binary_boot_logo_rgb565_start");
static const lv_image_dsc_t boot_logo = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .w = BOOT_LOGO_WIDTH,
        .h = BOOT_LOGO_HEIGHT,
        .stride = BOOT_LOGO_WIDTH * 2,
    },
    .data_size = BOOT_LOGO_WIDTH * BOOT_LOGO_HEIGHT * 2,
    .data = boot_logo_data,
};

/* Both callers hold LVGL exclusively. Render the full screen and wait for its
 * final staged DMA transfer before reporting the frame as presented. */
static esp_err_t complete_boot_refresh(lv_display_t *display)
{
    app_display_metrics_t before, after;
    app_display_metrics_get(&before);
    lv_obj_invalidate(lv_display_get_screen_active(display));
    lv_refr_now(display);
    app_display_metrics_get(&after);
    const uint64_t target = after.submitted_sequence;
    const uint64_t frame_bytes = (uint64_t)lv_display_get_horizontal_resolution(display) *
        (uint64_t)lv_display_get_vertical_resolution(display) * sizeof(uint16_t);
    if (target <= before.submitted_sequence ||
        after.staging_copied_bytes - before.staging_copied_bytes < frame_bytes ||
        after.dropped_regions != before.dropped_regions ||
        after.flush_wait_timeouts != before.flush_wait_timeouts || after.staging_error != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t deadline = esp_timer_get_time() + 100000;
    while (after.completed_sequence < target) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
        app_display_metrics_get(&after);
    }
    return ESP_OK;
}

static esp_err_t present_boot_frame(lv_display_t *display, int64_t *logo_on_us)
{
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *logo = lv_image_create(screen);
    lv_image_set_src(logo, &boot_logo);
    lv_obj_center(logo);

    /* The initialized panel is blank until its entire first frame is ready. */
    esp_err_t error = complete_boot_refresh(display);
    if (error == ESP_OK) {
        error = esp_lcd_panel_disp_on_off(s_board.display, true);
        if (error == ESP_OK) *logo_on_us = esp_timer_get_time();
    }
    return error;
}

static lv_display_t *init_lvgl(int64_t *logo_on_us)
{
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_priority = 6;
    /* Keep GUI and audio service work off the S31 video decoder's core. */
    lvgl_cfg.task_affinity = 0;
    lvgl_cfg.task_max_sleep_ms = 8;
    lvgl_cfg.timer_period_ms = 2;
    ESP_LOGI(TAG, "LVGL scheduling: tick=%d ms refresh=%d ms max_sleep=%d ms",
             lvgl_cfg.timer_period_ms, CONFIG_LV_DEF_REFR_PERIOD, lvgl_cfg.task_max_sleep_ms);
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = s_board.display_io,
        .panel_handle = s_board.display,
        .buffer_size = CONFIG_AETHER_LCD_H_RES * CONFIG_AETHER_LCD_BUFFER_LINES,
        .double_buffer = true,
        .hres = CONFIG_AETHER_LCD_H_RES,
        .vres = CONFIG_AETHER_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rounder_cb = app_display_rounder,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
        },
    };
    /* The port uses a recursive mutex. Install completion/wait hooks before
     * its GUI task can submit the newly-created display's first transfer. */
    ESP_RETURN_ON_FALSE(lvgl_port_lock(1000), NULL, TAG, "display setup lock timeout");
    lv_display_t *display = lvgl_port_add_disp(&display_cfg);
    if (!display) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "add LVGL display failed");
        return NULL;
    }
    const esp_err_t metrics_error = app_display_metrics_attach_staged_rgb565(
        s_board.display_io, s_board.display, display,
        CONFIG_AETHER_LCD_H_RES * CONFIG_AETHER_LCD_BUFFER_LINES * sizeof(uint16_t));
    if (metrics_error != ESP_OK) {
        /* PSRAM draw buffers require our reserved internal DMA mirror. Never
         * fall back to implicit per-transfer SDK allocations under pressure. */
        lvgl_port_remove_disp(display);
        lvgl_port_unlock();
        ESP_LOGE(TAG, "reserved display DMA transport unavailable: %s", esp_err_to_name(metrics_error));
        return NULL;
    }
    const esp_err_t logo_error = present_boot_frame(display, logo_on_us);
    if (logo_error != ESP_OK) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "boot frame unavailable: %s", esp_err_to_name(logo_error));
        return NULL;
    }
    const esp_err_t sync_error = app_display_sync_attach(display, s_board.display_io);
    if (sync_error != ESP_OK) ESP_LOGW(TAG, "display TE sync unavailable: %s", esp_err_to_name(sync_error));
    lvgl_port_unlock();
    return display;
}

static void wait_for_logo_interval(int64_t logo_on_us)
{
    const int64_t deadline = logo_on_us + BOOT_LOGO_MIN_VISIBLE_MS * INT64_C(1000);
    int64_t remaining;
    while ((remaining = deadline - esp_timer_get_time()) > 0) {
        const TickType_t ticks = pdMS_TO_TICKS((remaining + 999) / 1000);
        vTaskDelay(ticks ? ticks : 1);
    }
}

void app_main(void)
{
    const int64_t app_start_us = esp_timer_get_time();
    const esp_err_t display_err = aether_board_init_display(&s_board);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "display bring-up failed: %s; startup halted",
                 esp_err_to_name(display_err));
        return;
    }
    ESP_RETURN_VOID_ON_FALSE(s_board.display_ready, TAG, "display is not available");
    const int64_t panel_ready_us = esp_timer_get_time();
    int64_t logo_on_us = 0;
    lv_display_t *display = init_lvgl(&logo_on_us);
    ESP_RETURN_VOID_ON_FALSE(display, TAG, "LVGL display is not available");
    ESP_LOGI(TAG, "boot display: panel_ready_ms=%lld logo_on_ms=%lld app_to_logo_ms=%lld",
             (long long)(panel_ready_us / 1000), (long long)(logo_on_us / 1000),
             (long long)((logo_on_us - app_start_us) / 1000));

    /* The software decoder logs its version on every lag-recovery reopen.
     * An unread native USB console can block that video task for50ms. */
    esp_log_level_set("H264_DEC.SW", ESP_LOG_WARN);
#ifndef AETHER_DISPLAY_PROFILING
    /* Detailed scrolling traces are opt-in. In normal use a disconnected USB
     * console must not add periodic blocking writes to the GUI task. */
    esp_log_level_set("display_perf", ESP_LOG_WARN);
    esp_log_level_set("touch_perf", ESP_LOG_WARN);
#endif
    ESP_ERROR_CHECK(init_nvs());
    check_flash_capacity();
#if CONFIG_SPIRAM
    const size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM initialized: %u MiB", (unsigned)(psram_size / (1024U * 1024U)));
    if (!esp_psram_is_initialized() || psram_size < 32U * 1024U * 1024U) {
        ESP_LOGW(TAG, "expected 32 MiB PSRAM, detected %u bytes", (unsigned)psram_size);
    }
#endif
    const esp_err_t board_err = aether_board_init_peripherals(&s_board);
    if (board_err != ESP_OK) {
        /* Rebooting would immediately retry the same possibly faulty load.
         * The board/PMIC failure paths perform their shutdown attempts first. */
        ESP_LOGE(TAG, "board initialization failed: %s; startup halted without automatic retry",
                 esp_err_to_name(board_err));
        return;
    }
    app_telemetry_warm_imu();
    app_telemetry_set_gyro_persistence_guard(media_flash_busy, NULL);
    ESP_ERROR_CHECK(app_telemetry_start(&s_board));

    lv_indev_t *touch_indev = NULL;

    ESP_ERROR_CHECK(aether_storage_new(&s_board, &s_storage));
    const esp_err_t wifi_error = aether_wifi_new(&s_wifi);
    if (wifi_error != ESP_OK) ESP_LOGW(TAG, "Wi-Fi unavailable: %s", esp_err_to_name(wifi_error));
    else aether_wifi_set_persistence_guard(s_wifi, media_flash_busy, NULL);
    esp_err_t video_error = aether_video_player_new(s_storage, s_board.audio, &s_video_player);
#if defined(AETHER_VIDEO_DIAGNOSTIC) || defined(AETHER_VIDEO_AUTOPROBE)
    if (video_error == ESP_OK) app_telemetry_set_media_probe(video_diagnostic_probe, s_video_player);
#endif
    if (video_error != ESP_OK) ESP_LOGW(TAG, "video player unavailable: %s", esp_err_to_name(video_error));
    const aether_bluetooth_config_t bluetooth_config = {.device_name = "Aether01"};
    const esp_err_t bluetooth_error = aether_bluetooth_new(&bluetooth_config, &s_bluetooth);
    if (bluetooth_error != ESP_OK) ESP_LOGW(TAG, "Bluetooth unavailable: %s", esp_err_to_name(bluetooth_error));
    if (s_board.audio_ready) {
#ifdef AETHER_AUDIO_DIAGNOSTIC
        /* Run before the volume service or any media client exists, so no
         * asynchronous preference apply can alter the known test gain. */
        const esp_err_t diagnostic_error = app_audio_diagnostic_run(s_board.audio);
        ESP_LOGI(TAG, "boot speaker diagnostic: %s", esp_err_to_name(diagnostic_error));
#endif
        esp_err_t system_error = app_audio_system_init(s_board.audio);
        if (system_error != ESP_OK) ESP_LOGW(TAG, "system audio unavailable: %s", esp_err_to_name(system_error));
        const esp_err_t recorder_error = aether_recorder_new(s_board.audio, s_storage, &s_recorder);
        if (recorder_error != ESP_OK) ESP_LOGW(TAG, "recorder unavailable: %s", esp_err_to_name(recorder_error));
        if (s_bluetooth) app_audio_system_set_bluetooth(s_bluetooth);
        esp_err_t err = aether_audio_player_new(s_board.audio, s_storage,
                                                &s_audio_player);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "audio player unavailable: %s", esp_err_to_name(err));
        } else {
            /* App volume controls all share the system master. Remove the
             * legacy hidden per-player attenuation before any stream starts.
             * Carry an explicitly muted old music preference into the master. */
            aether_audio_player_status_t previous_music;
            err = aether_audio_player_get_status(s_audio_player, &previous_music);
            if (err == ESP_OK) {
                if (system_error != ESP_OK) err = ac101s_audio_set_master_volume(s_board.audio, 70);
                if (err == ESP_OK && previous_music.volume == 0)
                    err = system_error == ESP_OK ? app_audio_system_set_volume(0, true) : ac101s_audio_set_master_volume(s_board.audio, 0);
                if (err == ESP_OK && previous_music.volume != 100) err = aether_audio_player_set_volume(s_audio_player, 100);
            }
            if (err != ESP_OK) ESP_LOGW(TAG, "music/system volume migration incomplete: %s", esp_err_to_name(err));
        }
    }
    if (s_board.touch_ready && s_board.touch) {
        esp_err_t err = aether_nes_new(s_storage, s_board.audio, s_board.touch,
                                       &s_nes);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "NES emulator unavailable: %s", esp_err_to_name(err));
        }
    }
    aether_audio_player_set_persistence_guard(s_audio_player, media_flash_busy, NULL);
    app_audio_system_set_persistence_guard(media_flash_busy, NULL);
    app_audio_system_set_media_control(remote_media_control, NULL);
    ESP_ERROR_CHECK(aether_usb_msc_init(s_storage));
    aether_usb_msc_set_vbus_query(usb_vbus_present,&s_board);

    /* Services above initialize while the logo is already lit. Only wait for
     * the remainder before enabling automatic dimming or blanking the panel. */
    wait_for_logo_interval(logo_on_us);
    ESP_RETURN_VOID_ON_FALSE(lvgl_port_lock(1000), TAG, "LVGL activity lock timeout");
    lv_display_trigger_activity(display);
    lvgl_port_unlock();
    ESP_ERROR_CHECK(app_power_init(&s_power, &s_board, display));
    app_pc_remote_init(s_bluetooth, &s_power, s_board.imu_ready, media_flash_busy);
    app_displacement_init(&s_power, s_board.imu_ready);
    app_power_set_persistence_guard(&s_power, media_flash_busy, NULL);
    const esp_err_t uac_error = aether_usb_audio_init();
    if (uac_error == ESP_OK) {
        const esp_err_t route_error = app_audio_system_set_usb_audio(true);
        if (route_error != ESP_OK) ESP_LOGW(TAG, "USB audio routing unavailable: %s", esp_err_to_name(route_error));
    } else ESP_LOGW(TAG, "USB audio manager unavailable: %s", esp_err_to_name(uac_error));
    const esp_err_t controls_error = app_nes_controls_init();
    if (controls_error != ESP_OK) ESP_LOGW(TAG, "NES key mapping unavailable: %s", esp_err_to_name(controls_error));
    app_nes_controls_set_persistence_guard(media_flash_busy, NULL);
    const esp_err_t otg_error = app_otg_init(&s_board, s_nes, &s_power);
    if (otg_error != ESP_OK) ESP_LOGW(TAG, "USB host unavailable: %s", esp_err_to_name(otg_error));
    const esp_err_t gps_error = app_gps_init(&s_power);
    if (gps_error != ESP_OK) ESP_LOGW(TAG, "GPS unavailable: %s", esp_err_to_name(gps_error));
    if (s_nes) ESP_ERROR_CHECK(aether_nes_set_input_gate(s_nes, nes_input_gate, &s_power));
    if (s_board.touch_ready) {
        touch_indev = app_touch_add(display, s_board.touch, &s_power);
        if (!touch_indev) ESP_LOGW(TAG, "LVGL touch input unavailable");
    }

    ESP_ERROR_CHECK(app_tools_init(&s_board));
    app_tools_set_persistence_guard(media_flash_busy, NULL);
    const esp_err_t utilities_error = app_utilities_init(&s_board, &s_power, media_flash_busy, NULL);
    if (utilities_error != ESP_OK)
        ESP_LOGW(TAG, "utilities services unavailable: %s", esp_err_to_name(utilities_error));
    ESP_RETURN_VOID_ON_FALSE(lvgl_port_lock(1000), TAG, "LVGL lock timeout");
    const esp_err_t input_error = app_usb_input_init(&s_power, display);
    if (input_error != ESP_OK) ESP_LOGW(TAG, "USB pointer/keypad unavailable: %s", esp_err_to_name(input_error));
    if (input_error == ESP_OK) app_usb_input_set_gate(usb_ui_input_gate, s_nes);
    esp_err_t ui_err = app_ui_start(&s_board, s_storage, s_audio_player,
                                    s_video_player, s_nes, touch_indev,
                                    &s_power, &s_ui);
    esp_err_t desktop_refresh = ui_err == ESP_OK ? complete_boot_refresh(display) : ui_err;
    const int64_t desktop_flush_us = esp_timer_get_time();
    lvgl_port_unlock();
    ESP_ERROR_CHECK(ui_err);
    if (desktop_refresh == ESP_OK) {
        ESP_LOGI(TAG, "boot display: desktop_flush_ms=%lld logo_interval_ms=%lld",
                 (long long)(desktop_flush_us / 1000), (long long)((desktop_flush_us - logo_on_us) / 1000));
    } else {
        ESP_LOGW(TAG, "boot desktop first refresh incomplete: %s", esp_err_to_name(desktop_refresh));
    }
    ESP_ERROR_CHECK(app_power_set_user_key_handler(&s_power, app_ui_queue_user_key, s_ui));
    ESP_ERROR_CHECK(app_power_set_user_key_press_handler(&s_power, intercom_user_key_press));
    ESP_ERROR_CHECK(app_power_set_shutdown_hook(&s_power, prepare_shutdown, &s_shutdown));
    if (wifi_error == ESP_OK) {
        const esp_err_t restore_error = aether_wifi_restore_preference(s_wifi);
        if (restore_error != ESP_OK) ESP_LOGW(TAG, "Wi-Fi startup restore: %s", esp_err_to_name(restore_error));
    }

    ESP_LOGI(TAG, "Aether01 UI ready: %u apps, TF hot-plug, screen idle power management",
             (unsigned)app_home_app_count());
    (void)app_pinyin_extended_init();
#ifdef AETHER_GPS_DIAGNOSTIC
    ESP_LOGI(TAG, "GPS one-shot input-only probe: %s", esp_err_to_name(app_gps_diagnostic_start()));
#endif
#ifdef AETHER_UI_SELFTEST
    ESP_LOGI(TAG, "on-target UI self-test: %s", esp_err_to_name(app_ui_selftest_start(s_ui)));
#endif
#ifdef AETHER_VIDEO_AUTOPROBE
    void (*probe_followup)(void *) = NULL;
#ifdef AETHER_RADIO_SELFTEST
    probe_followup = video_probe_radio_followup;
#endif
    ESP_LOGI(TAG, "one-shot video probe: %s", esp_err_to_name(app_video_probe_start(s_ui, probe_followup, NULL)));
#endif
#if defined(AETHER_RADIO_SELFTEST) && !defined(AETHER_VIDEO_AUTOPROBE)
    const esp_err_t radio_diag = app_radio_diagnostic_start(app_audio_system_bluetooth(), s_wifi);
    ESP_LOGI(TAG, "radio budget diagnostic: %s", esp_err_to_name(radio_diag));
#endif
#ifdef AETHER_CODEC_SELFTEST
    ESP_LOGI(TAG, "codec acceptance task: %s", esp_err_to_name(aether_codec_check_start()));
#endif
}
