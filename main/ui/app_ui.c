#include "app_ui_modal.h"
#include "app_ui_internal.h"
#include "app_battery.h"
#include "app_touch.h"
#include "app_display_metrics.h"
#include "app_telemetry.h"
#include "app_intercom.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"

const lv_font_t *app_ui_chinese_font(void)
{
    return app_ui_chinese_font_size(16);
}

const lv_font_t *app_ui_chinese_font_size(unsigned px)
{
    /* The supplied fonts contain Chinese text; LVGL symbols live in Montserrat. */
    static lv_font_t small, large;
    static bool initialized;
    if (!initialized) {
        small = font_16;
        large = font_24;
        small.fallback = &lv_font_montserrat_16;
        large.fallback = &lv_font_montserrat_24;
        initialized = true;
    }
    return px >= 24 ? &large : &small;
}

void app_ui_label_text(lv_obj_t *label, const char *text)
{
    const char *old = lv_label_get_text(label);
    if (text && old && strcmp(old, text) == 0) return;
    lv_label_set_text(label, text);
}

void app_ui_label_fmt(lv_obj_t *label, const char *format, ...)
{
    char text[512];
    va_list arguments, copy;
    va_start(arguments, format);
    va_copy(copy, arguments);
    const int length = vsnprintf(text, sizeof(text), format, copy);
    va_end(copy);
    if (length >= 0 && (size_t)length < sizeof(text)) {
        app_ui_label_text(label, text);
    } else if (length >= 0) {
        lv_label_set_text_vfmt(label, format, arguments);
    }
    va_end(arguments);
}

void app_ui_clear_timer(app_ui_t *ui)
{
    if (ui && ui->timer) {
        lv_timer_pause(ui->timer);
    }
}

void app_ui_set_timer(app_ui_t *ui, lv_timer_cb_t callback,
                      uint32_t period_ms)
{
    lv_timer_set_cb(ui->timer, callback);
    lv_timer_set_period(ui->timer, period_ms);
    lv_timer_reset(ui->timer);
    lv_timer_resume(ui->timer);
}

void app_ui_request_io(app_ui_t *ui, app_io_operation_t operation,
                        void (*complete)(app_ui_t *, const app_io_result_t *))
{
    const app_io_request_t request = {.operation = operation};
    app_ui_request_io_ex(ui, &request, complete);
}

void app_ui_request_io_ex(app_ui_t *ui, const app_io_request_t *request,
                         void (*complete)(app_ui_t *, const app_io_result_t *))
{
    app_io_request_t current = *request;
    current.generation = ui->page_generation;
    ui->io_complete = complete;
    const bool media_filter = current.filter == APP_IO_MUSIC || current.filter == APP_IO_VIDEO ||
                              current.filter == APP_IO_NES;
    ui->io_media_pending = current.operation == APP_IO_MUSIC || current.operation == APP_IO_VIDEO ||
        current.operation == APP_IO_NES || current.operation == APP_IO_RECORDER ||
        ((current.operation == APP_IO_CATALOG || current.operation == APP_IO_PREPARE) && media_filter);
    app_ui_media_control_gate(ui, ui->io_media_pending, ui->io_media_pending);
    lv_timer_set_period(ui->service_timer, 16);
    lv_timer_ready(ui->service_timer);
    const esp_err_t error = app_io_submit_request(ui->io, &current);
    if (error != ESP_OK) {
        ui->io_complete = NULL;
        ui->io_media_pending = false;
        app_io_result_t result = {.operation = current.operation,
                                 .generation = current.generation, .error = error};
        if (complete) complete(ui, &result);
        /* A completion callback may submit the next media preparation. */
        if (!ui->io_complete || !ui->io_media_pending) app_ui_media_control_gate(ui, false, false);
        if (current.operation != APP_IO_READER_LOAD && current.operation != APP_IO_READER_SAVE &&
            app_io_usb_storage_blocked(ui->io)) app_usb_storage_notice(ui);
    }
}

lv_obj_t *app_ui_add_text(lv_obj_t *parent, const char *text,
                          lv_align_t align, int32_t x, int32_t y,
                          uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font ? font : app_ui_chinese_font(), 0);
    lv_obj_align(label, align, x, y);
    return label;
}

static void back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        app_ui_show_home(lv_event_get_user_data(event));
    }
}

void app_ui_set_user_key_exit(app_ui_t *ui, bool enabled)
{
    if (!ui) return;
    portENTER_CRITICAL(&ui->media_control_lock);
    ui->user_key_exit_armed = enabled;
    ui->user_key_armed_at_us = esp_timer_get_time();
    ui->user_key_target_generation = ui->page_generation;
    if (!enabled) ui->user_key_exit_pending = false;
    portEXIT_CRITICAL(&ui->media_control_lock);
}
void app_ui_queue_user_key(void *context, int64_t pressed_at_us)
{
    app_ui_t *ui = context;
    if (!ui || app_ui_input_blocked(ui)) return;
    portENTER_CRITICAL(&ui->media_control_lock);
    if (ui->user_key_exit_armed && pressed_at_us >= ui->user_key_armed_at_us) {
        ui->user_key_requested_generation = ui->user_key_target_generation;
        ui->user_key_exit_pending = true;
        ui->user_key_exit_armed = false;
    }
    portEXIT_CRITICAL(&ui->media_control_lock);
}
static void service_user_key(app_ui_t *ui)
{
    portENTER_CRITICAL(&ui->media_control_lock);
    const bool pending = ui->user_key_exit_pending;
    const uint32_t generation = ui->user_key_requested_generation;
    ui->user_key_exit_pending = false;
    portEXIT_CRITICAL(&ui->media_control_lock);
    if (pending && generation == ui->page_generation && !app_ui_input_blocked(ui))
        app_ui_show_home(ui);
}

lv_obj_t *app_ui_new_screen(app_ui_t *ui)
{
    app_ui_set_user_key_exit(ui, false);
    ui->media_release_notice = NULL;
    ++ui->page_generation;
    app_io_cancel(ui->io, ui->page_generation);
    ui->io_complete = NULL;
    ui->io_media_pending = false;
    app_ui_media_control_gate(ui, false, true);
    app_telemetry_set_interest(0);
    if (ui->page_cleanup) {
        void (*cleanup)(app_ui_t *) = ui->page_cleanup;
        ui->page_cleanup = NULL;
        cleanup(ui);
    }
    ui->page_context = NULL;
    app_ui_clear_timer(ui);
    for (size_t i = 0; i < sizeof(ui->app_obj) / sizeof(ui->app_obj[0]); ++i) {
        ui->app_obj[i] = NULL;
    }
    ui->app_path[0] = '\0';
    ui->app_value = 0;
    ui->media_started = false;
    ui->shutdown_overlay = NULL;
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x070A12), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, app_ui_chinese_font(), 0);
    ui->screen = screen;
    return screen;
}

lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title)
{
    lv_obj_t *screen = app_ui_new_screen(ui);

    lv_obj_t *back = lv_button_create(screen);
    lv_obj_set_size(back, 72, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 14, 12);
    lv_obj_set_style_radius(back, 18, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x18243A), 0);
    lv_obj_add_event_cb(back, back_event_cb, LV_EVENT_CLICKED, ui);
    app_ui_add_text(back, LV_SYMBOL_LEFT, LV_ALIGN_CENTER, 0, 0, 0xBFD7FF,
                    &lv_font_montserrat_20);

    app_ui_add_text(screen, title, LV_ALIGN_TOP_MID, 0, 20, 0xEAF2FF,
                    app_ui_chinese_font_size(24));

    lv_obj_t *battery = app_battery_create(screen, 116);
    if (battery) lv_obj_align(battery, LV_ALIGN_TOP_RIGHT, -14, 20);

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_set_size(content, 448, 394);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(content, 24, 0);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x10192A), 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 20, 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    ui->content = content;

    lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
    return content;
}

static void shutdown_notice_close(lv_event_t *event)
{
    lv_msgbox_close(lv_event_get_user_data(event));
}

static const char *shutdown_stage_name(app_power_shutdown_stage_t stage)
{
    switch (stage) {
    case APP_POWER_SHUTDOWN_BEGIN: return "开始关机";
    case APP_POWER_SHUTDOWN_QUIESCE: return "停止采样";
    case APP_POWER_SHUTDOWN_SENSORS: return "传感器低功耗";
    case APP_POWER_SHUTDOWN_FINALIZE: return "保存与卸载 TF";
    case APP_POWER_SHUTDOWN_POWER_OFF: return "关闭电源";
    case APP_POWER_SHUTDOWN_CANCEL: return "恢复运行";
    default: return "准备";
    }
}

static void shutdown_overlay_show(app_ui_t *ui)
{
    if (ui->shutdown_overlay) return;
    ui->shutdown_overlay = lv_obj_create(ui->screen);
    lv_obj_set_size(ui->shutdown_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(ui->shutdown_overlay, 0, 0);
    lv_obj_remove_flag(ui->shutdown_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui->shutdown_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(ui->shutdown_overlay, 0, 0);
    lv_obj_set_style_border_width(ui->shutdown_overlay, 0, 0);
    lv_obj_set_style_bg_color(ui->shutdown_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui->shutdown_overlay, LV_OPA_80, 0);
    lv_obj_t *label = app_ui_add_text(ui->shutdown_overlay, "正在保存并准备关机…",
        LV_ALIGN_CENTER, 0, 0, 0xEAF2FF, app_ui_chinese_font_size(24));
    lv_obj_set_width(label, 408);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

static void app_ui_shutdown_service(app_ui_t *ui)
{
    app_power_status_t status;
    if (app_power_get_status(ui->power, &status) != ESP_OK) return;
    const bool io_ready = !app_io_is_quiescing(ui->io);
    /* This ACK must not depend on power recovery: CANCEL waits for it before
     * it can clear shutdown_recovering. Admission stays blocked separately. */
    if (atomic_load(&ui->storage_resume_pending) && io_ready)
        atomic_store(&ui->storage_resume_pending, false);
    if (status.shutdown_recovering) {
        shutdown_overlay_show(ui);
        app_ui_media_control_gate(ui, true, true);
        app_ui_label_fmt(lv_obj_get_child(ui->shutdown_overlay, 0),
            "关机已取消，正在恢复资源…\n%s",
            status.shutdown_cancel_error != ESP_OK ? esp_err_to_name(status.shutdown_cancel_error) :
                status.sensor_restore_error != ESP_OK ? esp_err_to_name(status.sensor_restore_error) : "请稍候");
        return;
    }
    if (io_ready && ui->shutdown_overlay &&
        status.shutdown_state != APP_POWER_SHUTDOWN_PREPARING &&
        status.shutdown_state != APP_POWER_SHUTDOWN_REQUESTED) {
        lv_obj_delete(ui->shutdown_overlay);
        ui->shutdown_overlay = NULL;
        app_ui_media_control_gate(ui, false, true);
    }
    if (status.shutdown_state == APP_POWER_SHUTDOWN_PREPARING) {
        ui->shutdown_failure_shown = false;
        if (ui->shutdown_overlay) app_ui_label_fmt(lv_obj_get_child(ui->shutdown_overlay, 0),
            "正在%s…", shutdown_stage_name(status.shutdown_stage));
    } else if (status.shutdown_state == APP_POWER_SHUTDOWN_REQUESTED) {
        if (ui->shutdown_overlay) app_ui_label_text(lv_obj_get_child(ui->shutdown_overlay, 0), "正在关闭电源…");
    } else if (status.shutdown_state == APP_POWER_SHUTDOWN_FAILED &&
               !ui->shutdown_failure_shown && !app_io_is_quiescing(ui->io)) {
        ui->shutdown_failure_shown = true;
        lv_obj_t *notice = app_ui_modal_msgbox(ui->screen);
        lv_obj_set_width(notice, 400);
        lv_obj_center(notice);
        lv_obj_set_style_text_font(notice, app_ui_chinese_font(), 0);
        lv_obj_t *title = lv_msgbox_add_title(notice, "关机未完成，保持开机");
        lv_obj_set_style_text_font(title, app_ui_chinese_font_size(24), 0);
        lv_msgbox_add_text_fmt(notice, "步骤：%s\n原因：%s%s%s\n%s",
            shutdown_stage_name(status.shutdown_stage), esp_err_to_name(status.shutdown_error),
            status.sensor_restore_error != ESP_OK ? "\n传感器恢复失败：" : "",
            status.sensor_restore_error != ESP_OK ? esp_err_to_name(status.sensor_restore_error) : "",
            app_io_usb_storage_blocked(ui->io) ? "请先在电脑弹出U盘并关闭虚拟U盘开关。" :
                "请检查 TF 卡和设备状态后重试；媒体保持停止。");
        lv_obj_t *close = lv_msgbox_add_footer_button(notice, "知道了");
        lv_obj_add_event_cb(close, shutdown_notice_close, LV_EVENT_CLICKED, notice);
        app_ui_modal_focus(notice, close);
    }
}

static void service_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    service_user_key(ui);
    app_io_result_t result;
    if (app_io_poll(ui->io, &result)) {
        if (result.generation == ui->page_generation && ui->io_complete) {
            /* Preparation may have completed just before USB/shutdown claimed
             * storage. Do not launch its media producer from a stale success. */
            if (result.error == ESP_OK && result.operation != APP_IO_READER_LOAD &&
                result.operation != APP_IO_READER_SAVE && app_io_usb_storage_blocked(ui->io))
                result.error = ESP_ERR_INVALID_STATE;
            void (*complete)(app_ui_t *, const app_io_result_t *) = ui->io_complete;
            ui->io_complete = NULL;
            ui->io_media_pending = false;
            complete(ui, &result);
            if (!ui->io_complete || !ui->io_media_pending) app_ui_media_control_gate(ui, false, false);
            if (result.error != ESP_OK && result.operation != APP_IO_READER_LOAD &&
                result.operation != APP_IO_READER_SAVE && app_io_usb_storage_blocked(ui->io))
                app_usb_storage_notice(ui);
        }
        app_io_result_free(&result);
    }
    bool releasing = false;
    esp_err_t release_error = ESP_OK;
    (void)app_io_media_release_status(ui->io, &releasing, &release_error);
    app_power_status_t input_power = {0};
    const bool power_blocked = app_power_get_status(ui->power,&input_power) != ESP_OK ||
        input_power.shutdown_recovering || input_power.shutdown_state == APP_POWER_SHUTDOWN_PREPARING ||
        input_power.shutdown_state == APP_POWER_SHUTDOWN_REQUESTED;
    app_ui_media_control_gate(ui, releasing || ui->io_media_pending || power_blocked || app_intercom_busy() ||
        app_io_usb_storage_blocked(ui->io), false);
    if (releasing && !power_blocked) {
        if (!ui->media_release_notice) {
            ui->media_release_notice = app_ui_add_text(ui->screen,"",LV_ALIGN_BOTTOM_MID,0,-8,
                0xFFD180,app_ui_chinese_font());
            lv_obj_set_size(ui->media_release_notice,440,24);
            lv_obj_set_style_text_align(ui->media_release_notice,LV_TEXT_ALIGN_CENTER,0);
            lv_obj_set_style_bg_color(ui->media_release_notice,lv_color_hex(0x10192A),0);
            lv_obj_set_style_bg_opa(ui->media_release_notice,LV_OPA_90,0);
        }
        if (release_error == ESP_OK) app_ui_label_text(ui->media_release_notice,"正在停止媒体并释放资源…");
        else app_ui_label_fmt(ui->media_release_notice,"资源释放重试：%s",esp_err_to_name(release_error));
    } else if (ui->media_release_notice) {
        lv_obj_delete(ui->media_release_notice);ui->media_release_notice=NULL;
    }
    app_ui_media_control_process(ui);
    bool measuring_media = false;
    app_ui_shutdown_service(ui);
    if (ui->media_active) {
        aether_video_status_t video;
        aether_nes_status_t nes;
        measuring_media = (ui->video_player &&
            aether_video_player_get_status(ui->video_player, &video) == ESP_OK &&
            video.state == AETHER_VIDEO_PLAYING) ||
            (ui->nes && aether_nes_get_status(ui->nes, &nes) == ESP_OK &&
             nes.state == AETHER_NES_RUNNING);
    }
    /* Intentional pauses/sleep are not display-stall samples. */
    app_display_metrics_set_active(measuring_media && !app_power_is_sleeping(ui->power));
    if (ui->media_active) app_frames_present(ui->frames, ui->app_obj[0]);
    if (ui->touch_restore_pending &&
        (!ui->nes || aether_nes_stop_wait(ui->nes, 0) == ESP_OK)) {
        ui->touch_restore_pending = false;
        if (ui->touch_indev) {
            lv_indev_reset(ui->touch_indev, NULL);
            app_touch_set_enabled(ui->touch_indev, true);
        }
    }
    lv_timer_set_period(timer, ui->media_active || ui->io_complete ||
                                ui->touch_restore_pending || releasing ? 8 : 100);
}

esp_err_t app_ui_quiesce_storage_begin(app_ui_t *ui)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    /* This reservation and USB enable use the same IO command mutex. */
    esp_err_t error = app_io_quiesce_begin(ui->io);
    if (error != ESP_OK) return error;
    if (!lvgl_port_lock(50)) {
        /* The power coordinator may continue to sensor sleep/power-off after
         * bounded GUI cleanup times out. Only its explicit CANCEL can reopen IO. */
        return ESP_ERR_TIMEOUT;
    }
    app_ui_media_control_gate(ui, true, true);
    app_ui_show_home(ui); /* Cleanup submits the reader's last dirty bookmark. */
    app_ui_media_control_gate(ui, true, true);
    ui->shutdown_failure_shown = false;
    shutdown_overlay_show(ui);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t app_ui_quiesce_storage_wait(app_ui_t *ui, uint32_t timeout_ms)
{
    return ui ? app_io_quiesce_wait(ui->io, timeout_ms) : ESP_ERR_INVALID_ARG;
}

esp_err_t app_ui_quiesce_storage_resume(app_ui_t *ui)
{
    if (!ui) return ESP_ERR_INVALID_ARG;
    const esp_err_t error = app_io_resume(ui->io);
    if (error == ESP_OK) atomic_store(&ui->storage_resume_pending, true);
    return error;
}

bool app_ui_input_blocked(const app_ui_t *ui)
{
    return !ui || app_io_is_quiescing(ui->io) || atomic_load(&ui->storage_resume_pending);
}

esp_err_t app_ui_start(aether_board_t *board,
                       aether_storage_handle_t storage,
                       aether_audio_player_handle_t audio_player,
                       aether_video_player_handle_t video_player,
                       aether_nes_handle_t nes,
                       lv_indev_t *touch_indev,
                       app_power_t *power,
                       app_ui_t **out_ui)
{
    ESP_RETURN_ON_FALSE(board && storage && power && out_ui,
                        ESP_ERR_INVALID_ARG, "app_ui", "invalid argument");
    app_ui_t *ui = calloc(1, sizeof(*ui));
    ESP_RETURN_ON_FALSE(ui, ESP_ERR_NO_MEM, "app_ui", "no memory");
    ui->board = board;
    ui->storage = storage;
    ui->audio_player = audio_player;
    ui->video_player = video_player;
    ui->nes = nes;
    ui->touch_indev = touch_indev;
    ui->power = power;
    atomic_init(&ui->storage_resume_pending, false);
    esp_err_t err = app_ui_media_control_init(ui);
    if (err != ESP_OK) { free(ui); return err; }
    err = app_frames_new(&ui->frames);
    if (err != ESP_OK) {
        app_ui_media_control_delete(ui);
        free(ui);
        return err;
    }
    ui->timer = lv_timer_create(NULL, 1000, ui);
    if (!ui->timer) {
        app_frames_delete(ui->frames);
        app_ui_media_control_delete(ui);
        free(ui);
        return ESP_ERR_NO_MEM;
    }
    lv_timer_pause(ui->timer);
    ui->service_timer = lv_timer_create(service_timer, 100, ui);
    if (!ui->service_timer) {
        lv_timer_delete(ui->timer);
        app_frames_delete(ui->frames);
        app_ui_media_control_delete(ui);
        free(ui);
        return ESP_ERR_NO_MEM;
    }
    err = app_io_start(storage, audio_player, video_player, nes, &ui->io);
    if (err != ESP_OK) {
        lv_timer_delete(ui->timer);
        lv_timer_delete(ui->service_timer);
        app_frames_delete(ui->frames);
        app_ui_media_control_delete(ui);
        free(ui);
        return err;
    }
    app_ui_show_home(ui);
    *out_ui = ui;
    return ESP_OK;
}
