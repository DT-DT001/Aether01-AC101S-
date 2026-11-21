#include "aether_media_fs.h"
#include "app_ui_internal.h"
#include "app_touch.h"
#include "app_audio_system.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "app_text.h"
#include "esp_err.h"
#include "esp_timer.h"

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool media_storage_ready(app_ui_t *ui)
{
    /* Remount precedes USB teardown completion; remember only usable states. */
    return aether_media_fs_path_ready(ui->storage, aether_media_fs_default_root(ui->storage)) && !app_io_usb_storage_blocked(ui->io);
}

static void present_media_frame(app_ui_t *ui, const uint16_t *pixels,
                                uint16_t width, uint16_t height)
{
    if (ui) app_frames_push(ui->frames, pixels, width, height);
}

static void video_frame_cb(const uint16_t *pixels, uint16_t width,
                           uint16_t height, uint32_t frame_number,
                           void *user_ctx)
{
    (void)frame_number;
    present_media_frame(user_ctx, pixels, width, height);
}

typedef struct {
    bool expanded_error, preparation_required, controls_visible;
    bool dragging, seek_pending, background_pressed, background_moved;
    uint32_t duration_ms, seek_ms;
    bool seekable;
    int64_t interaction_us, notice_until_us;
    lv_point_t press_point;
    lv_obj_t *header, *controls, *time, *progress, *volume, *volume_label;
    lv_obj_t *speed, *play, *mute, *mute_label;
    char notice[96];
} video_page_t;

static bool video_is_mp4(const char *path)
{
    const char *extension = strrchr(path, '.');
    return extension && (!strcasecmp(extension, ".mp4") || !strcasecmp(extension, ".m4v"));
}

static lv_obj_t *media_panel(lv_obj_t *parent, int x, int y, int w, int h, lv_opa_t opacity)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y); lv_obj_set_size(object, w, h);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x060C16), 0);
    lv_obj_set_style_bg_opa(object, opacity, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    return object;
}

static lv_obj_t *media_fullscreen(app_ui_t *ui, const char *title)
{
    app_ui_begin_page(ui, title); /* Retain common cancellation/cleanup ownership. */
    lv_obj_clean(ui->screen);
    lv_obj_set_style_bg_color(ui->screen, lv_color_black(), 0);
    ui->content = media_panel(ui->screen, 0, 0, 480, 480, LV_OPA_COVER);
    lv_obj_set_style_bg_color(ui->content, lv_color_black(), 0);
    return ui->content;
}

static void media_contain_ext_draw(lv_event_t *event)
{
    lv_obj_t *image = lv_event_get_target_obj(event);
    /* LVGL otherwise scales the already-fitted widget again for its dirty box.
     * These unrotated CONTAIN images draw inside the widget; retain AA margin. */
    if (lv_image_get_inner_align(image) != LV_IMAGE_ALIGN_CONTAIN || lv_image_get_rotation(image) ||
        lv_image_get_offset_x(image) || lv_image_get_offset_y(image) ||
        lv_obj_get_style_shadow_width(image, LV_PART_MAIN) ||
        lv_obj_get_style_outline_width(image, LV_PART_MAIN)) return;
    int32_t *extent = lv_event_get_param(event);
    if (extent) *extent = 2;
}

static void media_back(lv_event_t *event)
{
    app_ui_show_home(lv_event_get_user_data(event));
}

static lv_obj_t *media_button(lv_obj_t *parent, const char *text, int x, int y,
                              int width, lv_event_cb_t callback, app_ui_t *ui)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y); lv_obj_set_size(button, width, 44);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x263C55), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, ui);
    return app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, app_ui_chinese_font());
}

static void video_controls(app_ui_t *ui, bool visible)
{
    video_page_t *page = ui->page_context;
    if (!page || page->controls_visible == visible) return;
    page->controls_visible = visible;
    lv_obj_t *panels[] = {page->header, page->controls};
    for (unsigned i = 0; i < sizeof(panels) / sizeof(panels[0]); ++i) {
        if (visible) lv_obj_remove_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void video_activity(app_ui_t *ui)
{
    video_page_t *page = ui->page_context;
    if (page) { page->interaction_us = esp_timer_get_time(); video_controls(ui, true); }
}

static void video_control_activity(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED ||
        code == LV_EVENT_VALUE_CHANGED) video_activity(lv_event_get_user_data(event));
}

static void video_background(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    video_page_t *page = ui->page_context;
    if (!page || lv_event_get_target_obj(event) != ui->app_obj[4]) return;
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *input = lv_indev_active(); lv_point_t point = {0};
    if (input) lv_indev_get_point(input, &point);
    if (code == LV_EVENT_PRESSED) {
        page->press_point = point; page->background_moved = false; page->background_pressed = true;
        page->interaction_us = esp_timer_get_time();
    } else if (code == LV_EVENT_PRESSING && page->background_pressed) {
        if (abs(point.x - page->press_point.x) > 12 || abs(point.y - page->press_point.y) > 12)
            page->background_moved = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET) {
        page->background_pressed = false;
    } else if (code == LV_EVENT_CLICKED && !page->background_moved) {
        aether_video_status_t status = {0};
        const bool playing = ui->media_started && aether_video_player_get_status(ui->video_player, &status) == ESP_OK &&
                             status.state == AETHER_VIDEO_PLAYING && !page->expanded_error;
        page->interaction_us = esp_timer_get_time();
        video_controls(ui, playing ? !page->controls_visible : true);
    }
}

static void video_notice(app_ui_t *ui, const char *action, esp_err_t error)
{
    video_page_t *page = ui->page_context;
    if (!page) return;
    /* Keep the complete API code readable in the one-line overlay. */
    snprintf(page->notice, sizeof(page->notice), "%s：0x%X", action, (unsigned)error);
    page->notice_until_us = esp_timer_get_time() + 3000000;
    video_activity(ui);
}

static void video_error_layout(app_ui_t *ui, bool expanded)
{
    video_page_t *page = ui->page_context;
    if (!page || page->expanded_error == expanded) return;
    page->expanded_error = expanded;
    if (expanded) {
        lv_obj_add_flag(ui->app_obj[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_parent(ui->app_obj[1], ui->screen);
        lv_obj_set_pos(ui->app_obj[1], 24, 76); lv_obj_set_size(ui->app_obj[1], 432, 260);
        lv_label_set_long_mode(ui->app_obj[1], LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(page->controls, 0, 344); lv_obj_set_height(page->controls, 136);
        lv_obj_add_flag(page->time, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(page->progress, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(page->speed, 12); lv_obj_set_y(page->play, 12); lv_obj_set_y(page->mute, 12);
        lv_obj_set_y(page->volume, 92); lv_obj_set_y(page->volume_label, 82);
        lv_obj_add_state(page->speed, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_flag(ui->app_obj[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_parent(ui->app_obj[1], page->header);
        lv_obj_set_pos(ui->app_obj[1], 88, 38); lv_obj_set_size(ui->app_obj[1], 300, 20);
        lv_label_set_long_mode(ui->app_obj[1], LV_LABEL_LONG_DOT);
        lv_obj_set_pos(page->controls, 0, 304); lv_obj_set_height(page->controls, 176);
        lv_obj_remove_flag(page->time, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(page->progress, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(page->speed, 76); lv_obj_set_y(page->play, 76); lv_obj_set_y(page->mute, 76);
        lv_obj_set_y(page->volume, 142); lv_obj_set_y(page->volume_label, 132);
        if (!video_is_mp4(ui->app_path)) lv_obj_remove_state(page->speed, LV_STATE_DISABLED);
    }
}

static void video_show_error(app_ui_t *ui, const char *action, esp_err_t error, const char *detail)
{
    video_page_t *page = ui->page_context;
    ui->media_active = false; ui->media_started = false;
    if (page) { page->seek_pending = false; page->dragging = false; }
    app_frames_set_active(ui->frames, false); video_error_layout(ui, true); video_activity(ui);
    const char *hint = video_is_mp4(ui->app_path) ?
        "MP4：H.264 受限基线；音轨仅 AAC-LC\n尺寸≤480x480、源帧率≤30fps。\n可先在电脑转换后重试，或返回选择其他视频。" :
        "MJPEG 需基线 JPEG，最高 480x480\n宽高须为16倍数；AVI 暂不播放音轨。\n可先在电脑转换后重试，或返回选择其他视频。";
    app_ui_label_fmt(ui->app_obj[1], "%s：%s%s%s\n\n%s", action, esp_err_to_name(error),
        detail && detail[0] ? "\n" : "", detail ? detail : "", hint);
    app_ui_label_text(ui->app_obj[2], "重试");
}

static void video_reset_frame(void *context)
{
    app_ui_t *ui = context;
    app_frames_set_active(ui->frames, false);
    lv_image_set_src(ui->app_obj[0], NULL);
    app_frames_set_active(ui->frames, true);
}

static void video_cleanup(app_ui_t *ui)
{
    ui->media_active = false; app_frames_set_active(ui->frames, false);
    if (ui->app_obj[0]) lv_image_set_src(ui->app_obj[0], NULL);
    if (ui->video_player) { aether_video_player_stop(ui->video_player); aether_video_player_release_frame(ui->video_player); }
    (void)app_io_release_media(ui->io);
    free(ui->page_context); ui->page_context = NULL;
}

static esp_err_t video_start(app_ui_t *ui)
{
    video_page_t *page = ui->page_context;
    page->seek_pending = page->dragging = false; page->notice_until_us = 0;
    page->seekable = false; lv_obj_add_state(page->progress, LV_STATE_DISABLED);
    video_error_layout(ui, false); video_activity(ui);
    app_ui_label_text(ui->app_obj[1], "正在初始化解码器…"); app_ui_label_text(ui->app_obj[2], "暂停");
    ui->media_active = true; ui->media_started = true; video_reset_frame(ui);
    esp_err_t error = aether_video_player_play(ui->video_player, ui->app_path, (uint8_t)ui->app_value, video_frame_cb, ui);
    if (error != ESP_OK) video_show_error(ui, "启动失败", error, NULL);
    return error;
}

static void video_prepare(app_ui_t *ui);
static void video_action_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event); video_page_t *page = ui->page_context;
    if (!page || ui->io_complete || !ui->app_path[0] || !ui->video_player) return;
    video_activity(ui);
    if (!ui->media_started) { if (page->preparation_required) video_prepare(ui); else video_start(ui); return; }
    aether_video_status_t status;
    if (aether_video_player_get_status(ui->video_player, &status) != ESP_OK) return;
    esp_err_t error = ESP_OK;
    if (status.state == AETHER_VIDEO_PLAYING) error = aether_video_player_set_paused(ui->video_player, true);
    else if (status.state == AETHER_VIDEO_PAUSED) error = aether_video_player_set_paused(ui->video_player, false);
    else video_start(ui);
    if (error != ESP_OK) video_notice(ui, "暂停切换失败", error);
}

static void time_text(char *text, size_t capacity, uint32_t ms)
{
    const uint32_t seconds = ms / 1000;
    if (seconds >= 3600) snprintf(text, capacity, "%lu:%02lu:%02lu", (unsigned long)(seconds / 3600),
        (unsigned long)(seconds / 60 % 60), (unsigned long)(seconds % 60));
    else snprintf(text, capacity, "%02lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
}

static void video_time(app_ui_t *ui, uint32_t position_ms, uint32_t duration_ms)
{
    video_page_t *page = ui->page_context; char at[24], total[24];
    time_text(at, sizeof(at), position_ms); time_text(total, sizeof(total), duration_ms);
    if (duration_ms) app_ui_label_fmt(page->time, "%s / %s", at, total);
    else app_ui_label_fmt(page->time, "已播 %s · 总时长未知", at);
}

static void video_seek_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    /* LV_EVENT_ALL includes draw/style events on each translucent video redraw. */
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_VALUE_CHANGED &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST && code != LV_EVENT_INDEV_RESET) return;
    app_ui_t *ui = lv_event_get_user_data(event); video_page_t *page = ui->page_context;
    if (!page) return;
    if (code == LV_EVENT_INDEV_RESET) {
        if (page->dragging) page->seek_pending = false;
        page->dragging = false;
        return;
    }
    if (!page->seekable || !page->duration_ms) return;
    video_activity(ui);
    if (code == LV_EVENT_PRESSED) page->dragging = true;
    if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED) {
        page->seek_ms = (uint32_t)((uint64_t)(uint32_t)lv_slider_get_value(page->progress) * page->duration_ms / 1000U);
        if (page->seek_ms >= page->duration_ms) page->seek_ms = page->duration_ms - 1U;
        video_time(ui, page->seek_ms, page->duration_ms);
    }
    if (code == LV_EVENT_RELEASED) { page->dragging = false; page->seek_pending = true; }
    if (code == LV_EVENT_PRESS_LOST) page->dragging = false;
}

#ifdef AETHER_VIDEO_AUTOPROBE
esp_err_t app_ui_video_probe_seek(app_ui_t *ui, uint32_t position_ms)
{
    if (!ui || ui->page_cleanup != video_cleanup || !ui->media_started || app_ui_input_blocked(ui))
        return ESP_ERR_INVALID_STATE;
    video_page_t *page = ui->page_context;
    if (!page || !page->seekable || !page->duration_ms) return ESP_ERR_INVALID_STATE;
    video_activity(ui); /* Match the real slider's translucent control load. */
    page->seek_ms = position_ms < page->duration_ms ? position_ms : page->duration_ms - 1U;
    page->dragging = false;
    page->seek_pending = true;
    video_time(ui, page->seek_ms, page->duration_ms);
    return ESP_OK;
}
#endif

static void video_volume_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event); video_page_t *page = ui->page_context;
    if (!page) return;
    video_activity(ui);
    const esp_err_t error = app_audio_system_set_volume((uint8_t)lv_slider_get_value(page->volume),
        lv_event_get_code(event) == LV_EVENT_RELEASED);
    if (error != ESP_OK) video_notice(ui, "音量调整失败", error);
}

static void video_mute_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event); video_activity(ui);
    const esp_err_t error = app_audio_system_toggle_mute(true);
    if (error != ESP_OK) video_notice(ui, "静音切换失败", error);
}

static void video_audio(app_ui_t *ui)
{
    video_page_t *page = ui->page_context; app_audio_system_snapshot_t audio;
    app_audio_system_get_snapshot(&audio);
    if (!lv_obj_has_state(page->volume, LV_STATE_PRESSED)) lv_slider_set_value(page->volume, audio.master_volume, LV_ANIM_OFF);
    app_ui_label_fmt(page->volume_label, "%u%%", audio.master_volume);
    app_ui_label_text(page->mute_label, audio.master_volume ? "静音" : "恢复");
    if (audio.ready) { lv_obj_remove_state(page->volume, LV_STATE_DISABLED); lv_obj_remove_state(page->mute, LV_STATE_DISABLED); }
    else { lv_obj_add_state(page->volume, LV_STATE_DISABLED); lv_obj_add_state(page->mute, LV_STATE_DISABLED); }
}

static void video_status_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer); video_page_t *page = ui->page_context;
    if (!page) return;
    video_audio(ui);
    if (!ui->media_started) return; /* Do not overwrite prepare/start errors. */
    aether_video_status_t status;
    if (aether_video_player_get_status(ui->video_player, &status) != ESP_OK) return;
    const bool running = status.state == AETHER_VIDEO_PLAYING || status.state == AETHER_VIDEO_PAUSED;
    page->duration_ms = status.duration_ms;
    page->seekable = running && status.seek_supported && status.duration_ms && !page->expanded_error;
    if (page->seekable) lv_obj_remove_state(page->progress, LV_STATE_DISABLED);
    else lv_obj_add_state(page->progress, LV_STATE_DISABLED);
    if (page->seek_pending && !page->dragging) {
        if (!page->seekable) page->seek_pending = false;
        else {
            const esp_err_t error = aether_video_player_seek_with_reset(ui->video_player, page->seek_ms, video_reset_frame, ui);
            if (error == ESP_OK) { page->seek_pending = false; status.seeking = true; status.seek_requested_ms = page->seek_ms; }
            else if (error != ESP_ERR_INVALID_STATE) { page->seek_pending = false; video_notice(ui, "提交跳转失败", error); }
        }
    }
    uint32_t position = status.duration_ms ? (status.seeking ? status.seek_requested_ms : status.position_ms) : status.elapsed_ms;
    if (page->dragging || page->seek_pending) position = page->seek_ms;
    video_time(ui, position, status.duration_ms);
    if (!page->dragging) lv_slider_set_value(page->progress, status.duration_ms ?
        (int32_t)((uint64_t)position * 1000U / status.duration_ms) : 0, LV_ANIM_OFF);
    switch (status.state) {
    case AETHER_VIDEO_STARTING:
        app_ui_label_text(ui->app_obj[1], "正在初始化解码器…"); app_ui_label_text(ui->app_obj[2], "暂停"); break;
    case AETHER_VIDEO_PLAYING:
        if (status.seeking || page->seek_pending) app_ui_label_text(ui->app_obj[1], "正在跳转…");
        else if (status.width && status.height) app_ui_label_fmt(ui->app_obj[1], "%ux%u · 播放中", status.width, status.height);
        else app_ui_label_text(ui->app_obj[1], "等待首帧…");
        /* Metrics remain cached in the player/display. Never print from this
         * LVGL callback: an absent USB console must not stall touch/render. */
        app_ui_label_text(ui->app_obj[2], "暂停");
        break;
    case AETHER_VIDEO_PAUSED:
        app_ui_label_text(ui->app_obj[1], status.seeking || page->seek_pending ? "已暂停 · 正在跳转…" : "已暂停");
        app_ui_label_text(ui->app_obj[2], "继续"); break;
    case AETHER_VIDEO_FINISHED:
        app_ui_label_text(ui->app_obj[1], "播放完毕"); app_ui_label_text(ui->app_obj[2], "重播"); break;
    case AETHER_VIDEO_ERROR:
        video_show_error(ui, "播放错误", status.last_error, status.detail); return;
    default:
        app_ui_label_text(ui->app_obj[1], "已停止"); app_ui_label_text(ui->app_obj[2], "播放"); break;
    }
    const int64_t now = esp_timer_get_time();
    if (page->notice_until_us > now) app_ui_label_text(ui->app_obj[1], page->notice);
    const bool keep = status.state != AETHER_VIDEO_PLAYING || status.seeking || page->seek_pending || page->dragging ||
        !lv_image_get_src(ui->app_obj[0]) || page->notice_until_us > now;
    if (keep) video_controls(ui, true);
    else if (!page->background_pressed && !lv_obj_has_state(page->volume, LV_STATE_PRESSED) &&
             now - page->interaction_us >= 3000000) video_controls(ui, false);
}

static void video_loaded(app_ui_t *ui, const app_io_result_t *result)
{
    video_page_t *page = ui->page_context;
    if (!page) return;
    if (result->error != ESP_OK || !ui->video_player) {
        page->preparation_required = true;
        video_show_error(ui, "视频准备失败", result->error != ESP_OK ? result->error : ESP_ERR_INVALID_STATE, NULL); return;
    }
    page->preparation_required = false; strlcpy(ui->app_path, result->path, sizeof(ui->app_path));
    video_start(ui);
}

static void video_prepare(app_ui_t *ui)
{
    video_error_layout(ui, false); video_activity(ui);
    app_ui_label_text(ui->app_obj[1], "正在准备视频…"); app_ui_label_text(ui->app_obj[2], "等待");
    app_io_request_t request = {.operation = APP_IO_PREPARE, .filter = APP_IO_VIDEO};
    strlcpy(request.path, ui->app_path, sizeof(request.path)); app_ui_request_io_ex(ui, &request, video_loaded);
}

static void video_speed_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event); video_page_t *page = ui->page_context;
    if (!page || page->expanded_error || video_is_mp4(ui->app_path)) return;
    video_activity(ui);
    const uint32_t fps = ui->app_value == 0 ? 24 : ui->app_value == 24 ? 30 :
                         ui->app_value == 30 ? 10 : ui->app_value == 10 ? 15 : 0;
    const esp_err_t error = aether_video_player_set_fps(ui->video_player, (uint8_t)fps);
    if (error != ESP_OK) { video_notice(ui, "帧率设置失败", error); return; }
    ui->app_value = fps;
    if (fps) app_ui_label_fmt(ui->app_obj[3], "%lu FPS", (unsigned long)fps);
    else app_ui_label_text(ui->app_obj[3], "自动帧率");
}

static void video_stop_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event); video_page_t *page = ui->page_context;
    if (!page || ui->io_complete) return;
    const esp_err_t error = aether_video_player_stop(ui->video_player);
    if (error != ESP_OK) { video_show_error(ui, "停止失败", error, NULL); return; }
    page->seek_pending = page->dragging = false;
    page->seekable = false; lv_obj_add_state(page->progress, LV_STATE_DISABLED);
    ui->media_active = false; ui->media_started = false; app_frames_set_active(ui->frames, false);
    video_error_layout(ui, false); video_activity(ui);
    app_ui_label_text(ui->app_obj[1], "已停止"); app_ui_label_text(ui->app_obj[2], "播放");
}

void app_open_video_path(app_ui_t *ui, const char *path)
{
    char selected[192]; strlcpy(selected, path, sizeof(selected));
    lv_obj_t *content = media_fullscreen(ui, "视频播放器");
    ui->app_value = 0;
    if (!ui->video_player) {
        media_button(ui->screen, "返回", 12, 12, 64, media_back, ui);
        app_ui_add_text(content, "视频播放器未就绪", LV_ALIGN_CENTER, 0, 0, 0xFF8888, NULL); return;
    }
    video_page_t *page = calloc(1, sizeof(*page));
    if (!page) {
        media_button(ui->screen, "返回", 12, 12, 64, media_back, ui);
        app_ui_add_text(content, "内存不足，请返回后重试", LV_ALIGN_CENTER, 0, 0, 0xFF8888, NULL); return;
    }
    page->preparation_required = true; page->controls_visible = true;
    ui->page_context = page; ui->page_cleanup = video_cleanup; strlcpy(ui->app_path, selected, sizeof(ui->app_path));
    ui->app_obj[4] = content;
    lv_obj_add_event_cb(content, video_background, LV_EVENT_ALL, ui);
    ui->app_obj[0] = lv_image_create(content); lv_obj_set_size(ui->app_obj[0], 480, 480);
    lv_obj_add_event_cb(ui->app_obj[0], media_contain_ext_draw, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    app_frames_set_viewport(ui->frames, 480, 480);
    lv_obj_remove_flag(ui->app_obj[0], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_image_set_inner_align(ui->app_obj[0], LV_IMAGE_ALIGN_CONTAIN); lv_obj_center(ui->app_obj[0]);
    page->header = media_panel(ui->screen, 0, 0, 480, 70, LV_OPA_70);
    media_button(page->header, "返回", 12, 12, 64, media_back, ui);
    lv_obj_t *name = app_ui_add_text(page->header, base_name(selected), LV_ALIGN_TOP_LEFT, 88, 12, 0xEAF2FF, NULL);
    lv_obj_set_size(name, 300, 20); lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    ui->app_obj[1] = app_ui_add_text(page->header, "正在准备视频…", LV_ALIGN_TOP_LEFT, 88, 38, 0xAFC6E9, NULL);
    lv_obj_set_size(ui->app_obj[1], 300, 20); lv_label_set_long_mode(ui->app_obj[1], LV_LABEL_LONG_DOT);
    lv_obj_t *stop = media_button(page->header, "停止", 404, 12, 64, video_stop_event, ui);
    lv_obj_add_event_cb(lv_obj_get_parent(stop), video_control_activity, LV_EVENT_ALL, ui);
    page->controls = media_panel(ui->screen, 0, 304, 480, 176, LV_OPA_70);
    page->time = app_ui_add_text(page->controls, "总时长未知", LV_ALIGN_TOP_LEFT, 24, 10, 0xEAF2FF, NULL);
    lv_obj_set_size(page->time, 432, 20);
    page->progress = lv_slider_create(page->controls); lv_obj_set_pos(page->progress, 24, 46);
    lv_obj_set_size(page->progress, 432, 10); lv_slider_set_range(page->progress, 0, 1000);
    lv_obj_set_ext_click_area(page->progress, 14); lv_obj_add_state(page->progress, LV_STATE_DISABLED);
    lv_obj_remove_flag(page->progress, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(page->progress, video_seek_event, LV_EVENT_ALL, ui);
    ui->app_obj[3] = media_button(page->controls, video_is_mp4(selected) ? "原速 MP4" : "自动帧率", 20, 76, 112, video_speed_event, ui);
    page->speed = lv_obj_get_parent(ui->app_obj[3]);
    if (video_is_mp4(selected)) lv_obj_add_state(page->speed, LV_STATE_DISABLED);
    ui->app_obj[2] = media_button(page->controls, "等待", 177, 76, 126, video_action_event, ui);
    page->play = lv_obj_get_parent(ui->app_obj[2]);
    page->mute_label = media_button(page->controls, "静音", 348, 76, 112, video_mute_event, ui);
    page->mute = lv_obj_get_parent(page->mute_label);
    page->volume = lv_slider_create(page->controls); lv_obj_set_pos(page->volume, 24, 142);
    lv_obj_set_size(page->volume, 354, 12); lv_slider_set_range(page->volume, 0, 100);
    lv_obj_set_ext_click_area(page->volume, 12); lv_obj_remove_flag(page->volume, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(page->volume, video_volume_event, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(page->volume, video_volume_event, LV_EVENT_RELEASED, ui);
    page->volume_label = app_ui_add_text(page->controls, "", LV_ALIGN_TOP_LEFT, 398, 132, 0xD9E8FF, NULL);
    lv_obj_set_size(page->volume_label, 62, 24);
    lv_obj_t *activity[] = {page->header, page->controls, page->play, page->speed, page->mute, page->volume};
    for (unsigned i = 0; i < sizeof(activity) / sizeof(activity[0]); ++i)
        lv_obj_add_event_cb(activity[i], video_control_activity, LV_EVENT_ALL, ui);
    app_ui_set_timer(ui, video_status_timer, 100); video_audio(ui); video_prepare(ui);
}

static void nes_frame_cb(const uint16_t *pixels, uint16_t width,
                         uint16_t height, uint32_t frame_number,
                         void *user_ctx)
{
    (void)frame_number;
    present_media_frame(user_ctx, pixels, width, height);
}

static void nes_cleanup(app_ui_t *ui)
{
    ui->media_active = false;
    app_frames_set_active(ui->frames, false);
    if (ui->app_obj[0]) lv_image_set_src(ui->app_obj[0], NULL);
    if (ui->nes) {
        aether_nes_stop(ui->nes);
        aether_nes_release_frame(ui->nes);
    }
    (void)app_io_release_media(ui->io);
    ui->touch_restore_pending = true;
}

static void nes_status_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    aether_nes_status_t status;
    if (aether_nes_get_status(ui->nes, &status) != ESP_OK) {
        return;
    }
    if (status.external_buttons && !app_power_is_sleeping(ui->power))
        lv_display_trigger_activity(lv_display_get_default());
    if (status.state == AETHER_NES_RUNNING || status.state == AETHER_NES_PAUSED) {
        /* A running game is only the viewport. HID owns joypad input; the
         * invisible upper-left hold gesture and HID EXIT still return home. */
        lv_obj_add_flag(ui->app_obj[1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->app_obj[2], LV_OBJ_FLAG_HIDDEN);
    } else if (status.state == AETHER_NES_ERROR) {
        if (ui->media_started) {
            ui->media_active = false;
            app_frames_set_active(ui->frames, false);
            ui->media_started = false;
            ui->touch_restore_pending = true;
        }
        lv_obj_remove_flag(ui->app_obj[1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->app_obj[2], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(ui->app_obj[1], 20, 84);
        lv_obj_set_size(ui->app_obj[1], 440, 204);
        lv_obj_set_style_text_align(ui->app_obj[1], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_bg_color(ui->app_obj[1], lv_color_hex(0x10192A), 0);
        lv_obj_set_style_bg_opa(ui->app_obj[1], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(ui->app_obj[1], 12, 0);
        lv_label_set_long_mode(ui->app_obj[1], LV_LABEL_LONG_WRAP);
        lv_obj_move_foreground(ui->app_obj[1]);
        app_ui_label_fmt(ui->app_obj[1], "无法运行此 ROM：%s\n%s\n\n请返回后选择其他 ROM。",
            esp_err_to_name(status.last_error), status.detail[0] ? status.detail : "读取或模拟器初始化失败");
    } else if (status.state == AETHER_NES_STOPPED && ui->media_started) {
        ui->media_started = false;
        app_ui_show_home(ui);
    }
}

static void nes_loaded(app_ui_t *ui, const app_io_result_t *result)
{
    if (result->error != ESP_OK || !ui->nes) {
        app_ui_label_fmt(ui->app_obj[1], "ROM不可用：%s", esp_err_to_name(result->error));
        return;
    }
    strlcpy(ui->app_path, result->path, sizeof(ui->app_path));
    ui->media_active = true;
    ui->media_started = true;
    ui->touch_restore_pending = false;
    app_frames_set_active(ui->frames, true);
    if (ui->touch_indev) app_touch_set_enabled(ui->touch_indev, false);
    app_ui_set_timer(ui, nes_status_timer, 200);
    esp_err_t err = aether_nes_start(ui->nes, ui->app_path, nes_frame_cb, ui);
    if (err != ESP_OK) {
        ui->media_active = false;
        ui->media_started = false;
        app_frames_set_active(ui->frames, false);
        ui->touch_restore_pending = true;
        app_ui_label_fmt(ui->app_obj[1], "启动失败：%s", esp_err_to_name(err));
    }
}

void app_open_nes_path(app_ui_t *ui, const char *path)
{
    char selected[192];
    strlcpy(selected, path, sizeof(selected));
    lv_obj_t *content = media_fullscreen(ui, "NES 模拟器");
    if (!ui->nes) {
        media_button(ui->screen, "返回", 14, 12, 72, media_back, ui);
        app_ui_add_text(content, "NES 未就绪，请检查触摸和存储", LV_ALIGN_CENTER, 0, 0, 0xFF8888, NULL); return;
    }
    lv_obj_t *viewport = content;
    ui->app_obj[0] = lv_image_create(viewport);
    lv_obj_set_size(ui->app_obj[0], 480, 480);
    lv_obj_add_event_cb(ui->app_obj[0], media_contain_ext_draw, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    app_frames_set_viewport(ui->frames, 480, 480);
    lv_image_set_inner_align(ui->app_obj[0], LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(ui->app_obj[0], false);
    lv_obj_remove_flag(ui->app_obj[0], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(ui->app_obj[0]);
    ui->app_obj[1] = app_ui_add_text(ui->screen, "正在加载ROM…",
                                     LV_ALIGN_TOP_LEFT, 20, 84, 0xFFCA5C,
                                     app_ui_chinese_font());
    lv_obj_set_size(ui->app_obj[1], 440, 204);
    lv_label_set_long_mode(ui->app_obj[1], LV_LABEL_LONG_WRAP);
    ui->app_obj[2] = lv_obj_get_parent(media_button(ui->screen, "返回", 14, 12, 72, media_back, ui));

    ui->page_cleanup = nes_cleanup;
    app_ui_set_user_key_exit(ui, true);
    app_io_request_t request = {.operation = APP_IO_PREPARE, .filter = APP_IO_NES};
    strlcpy(request.path, selected, sizeof(request.path));
    app_ui_request_io_ex(ui, &request, nes_loaded);
}

typedef struct {
    app_io_operation_t filter;
    app_io_entry_t *entries;
    size_t count, total;
    uint64_t offset;
    bool mounted;
    lv_obj_t *list, *info;
} media_picker_t;
static void picker_load(app_ui_t *ui);
static void picker_cleanup(app_ui_t *ui)
{
    media_picker_t *p = ui->page_context;
    if (p) { free(p->entries); free(p); }
    ui->page_context = NULL;
}
static void picker_select(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    media_picker_t *p = ui->page_context;
    size_t i = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (!p || ui->io_complete || i >= p->count) return;
    char path[192]; strlcpy(path, p->entries[i].path, sizeof(path));
    if (p->filter == APP_IO_VIDEO) app_open_video_path(ui, path);
    else app_open_nes_path(ui, path);
}
static void picker_loaded(app_ui_t *ui, const app_io_result_t *r)
{
    media_picker_t *p = ui->page_context;
    if (!p) return;
    free(p->entries); p->entries = NULL; p->count = 0;
    lv_obj_clean(p->list);
    lv_obj_set_height(p->list, 260);
    lv_obj_set_y(p->info, 268);
    if (r->error != ESP_OK) {
        app_ui_label_fmt(p->info, "读取失败：%s；插卡后刷新", esp_err_to_name(r->error)); return;
    }
    p->entries = malloc(r->count * sizeof(*p->entries));
    if (r->count && !p->entries) { app_ui_label_text(p->info, "内存不足"); return; }
    if (r->count) memcpy(p->entries, r->entries, r->count * sizeof(*p->entries));
    p->count = r->count; p->total = r->total_count;
    for (size_t i = 0; i < p->count; ++i) {
        char text[192]; strlcpy(text, base_name(p->entries[i].path), sizeof(text));
        app_text_sanitize_utf8(text, strlen(text));
        lv_obj_t *b = lv_list_add_button(p->list, NULL, text);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x18243A), 0);
        lv_obj_set_style_text_color(b, lv_color_hex(0xEAF2FF), 0);
        lv_obj_set_style_min_height(b, 44, 0);
        lv_obj_set_user_data(b, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(b, picker_select, LV_EVENT_CLICKED, ui);
    }
    app_ui_label_fmt(p->info, "%u 个文件 · 第 %llu 页%s", (unsigned)p->total,
        (unsigned long long)(p->offset / 32 + 1), p->total > 4096 ? "，仅索引前4096个" : "");
    if (!p->count) {
        if (p->filter == APP_IO_VIDEO) {
            lv_obj_set_height(p->list, 224);
            lv_obj_set_y(p->info, 232);
            app_ui_label_text(p->info,
                "MP4：H.264受限基线/AAC-LC\n尺寸≤480x480、源帧率≤30fps\nMJPEG：≤480x480，宽高16倍数\n视频放入 Video；可先在电脑转换");
        } else app_ui_label_text(p->info,
            "将 iNES 游戏放入 ROMs 文件夹；其他目录可从文件浏览打开");
    }
}
static void picker_load(app_ui_t *ui)
{
    media_picker_t *p = ui->page_context;
    app_io_request_t q = {.operation = APP_IO_CATALOG, .filter = p->filter, .offset = p->offset, .limit = 32};
    app_ui_label_text(p->info, "正在读取…"); app_ui_request_io_ex(ui, &q, picker_loaded);
}
static void picker_action(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    media_picker_t *p = ui->page_context;
    if (!p || ui->io_complete) return;
    intptr_t action = (intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (action < 0 && p->offset >= 32) p->offset -= 32;
    else if (action > 0 && p->offset + p->count < p->total && p->offset + p->count < 4096) p->offset += 32;
    picker_load(ui);
}
static void picker_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    media_picker_t *p = ui->page_context;
    bool mounted = media_storage_ready(ui);
    if (p && mounted != p->mounted) { p->mounted = mounted; picker_load(ui); }
}
static void media_picker(app_ui_t *ui, app_io_operation_t filter)
{
    lv_obj_t *content = app_ui_begin_page(ui, filter == APP_IO_VIDEO ? "选择视频" : "选择 NES 游戏");
    media_picker_t *p = calloc(1, sizeof(*p));
    if (!p) { app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF8888, NULL); return; }
    ui->page_context = p; ui->page_cleanup = picker_cleanup;
    p->filter = filter; p->mounted = media_storage_ready(ui);
    p->list = lv_list_create(content); lv_obj_set_size(p->list, 408, 260);
    lv_obj_set_style_bg_color(p->list, lv_color_hex(0x10192A), 0);
    lv_obj_set_style_border_width(p->list, 0, 0);
    p->info = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 268, 0xAFC6E9, NULL);
    lv_obj_set_width(p->info, 405);
    static const char *const names[] = {"上一页", "刷新", "下一页"};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *b = lv_button_create(content);
        lv_obj_set_pos(b, i * 140, 316); lv_obj_set_size(b, 128, 38);
        lv_obj_set_user_data(b, (void *)(intptr_t)(i - 1));
        lv_obj_add_event_cb(b, picker_action, LV_EVENT_CLICKED, ui);
        app_ui_add_text(b, names[i], LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, NULL);
    }
    app_ui_set_timer(ui, picker_timer, 500); picker_load(ui);
}
void app_open_video(app_ui_t *ui) { media_picker(ui, APP_IO_VIDEO); }
void app_open_nes(app_ui_t *ui) { media_picker(ui, APP_IO_NES); }
