#include "app_ui_internal.h"
#include "aether_recorder.h"
#include "app_audio_system.h"
#include "aether_media_fs.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { WAVE_SAMPLES = 36, WAVE_STEP = 9, WAVE_BAR_WIDTH = 4, WAVE_MAX_HEIGHT = 42 };

typedef struct {
    app_ui_t *ui;
    aether_recorder_handle_t recorder;
    uint32_t generation;
    bool pending, stopping, gated;
    uint8_t input_options;
    esp_err_t command_error;
    uint8_t peaks[WAVE_SAMPLES], wave_next, wave_count;
    uint64_t wave_duration;
    lv_obj_t *state, *time, *level, *waveform, *message;
    lv_obj_t *start, *pause, *pause_label, *stop, *replay, *browse;
    lv_obj_t *record_dot, *record_label;
    lv_obj_t *input;
} recorder_page_t;

static void update_page(recorder_page_t *page);
static void recorder_cleanup(app_ui_t *ui);

static recorder_page_t *event_page(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    return ui && ui->page_cleanup == recorder_cleanup ? ui->page_context : NULL;
}

static bool recorder_storage_ready(app_ui_t *ui)
{
    return aether_media_fs_path_ready(ui->storage, AETHER_STORAGE_MOUNT_POINT);
}

static void enabled(lv_obj_t *object, bool value)
{
    if (value) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static void gate(recorder_page_t *page, bool busy)
{
    /* Reassert the gate after the shared IO completion releases its preparation
     * gate. Only entering a new busy interval invalidates queued commands. */
    app_ui_media_control_gate(page->ui, busy, busy && !page->gated);
    page->gated = busy;
}

static void prepared(app_ui_t *ui, const app_io_result_t *result)
{
    if (ui->page_cleanup != recorder_cleanup) return;
    recorder_page_t *page = ui->page_context;
    if (!page || page->generation != ui->page_generation ||
        result->generation != page->generation) return;
    page->pending = false;
    page->command_error = result->error;
    if (result->error == ESP_OK) page->command_error = aether_recorder_start(page->recorder);
    update_page(page);
}

static void start_event(lv_event_t *event)
{
    recorder_page_t *page = event_page(event);
    aether_recorder_status_t status;
    if (!page || page->pending || page->stopping || !page->recorder ||
        aether_recorder_get_status(page->recorder, &status) != ESP_OK || status.active) return;
    app_audio_system_snapshot_t audio;
    app_audio_system_get_snapshot(&audio);
    if (!audio.ready || audio.input != audio.applied_input ||
        (audio.input == APP_AUDIO_INPUT_USB && !audio.usb_capture_connected)) return;
    page->pending = true;
    page->command_error = ESP_OK;
    gate(page, true);
    update_page(page);
    const app_io_request_t request = {.operation = APP_IO_RECORDER};
    app_ui_request_io_ex(page->ui, &request, prepared);
}

static void pause_event(lv_event_t *event)
{
    recorder_page_t *page = event_page(event);
    aether_recorder_status_t status;
    if (!page || page->pending || page->stopping || !page->recorder ||
        aether_recorder_get_status(page->recorder, &status) != ESP_OK ||
        (status.state != AETHER_RECORDER_RECORDING && status.state != AETHER_RECORDER_PAUSED)) return;
    page->command_error = aether_recorder_set_paused(page->recorder,
                                                    status.state != AETHER_RECORDER_PAUSED);
    update_page(page);
}

static void stop_event(lv_event_t *event)
{
    recorder_page_t *page = event_page(event);
    if (!page || page->pending || page->stopping || !page->recorder) return;
    page->command_error = aether_recorder_stop(page->recorder);
    if (page->command_error == ESP_OK) page->stopping = true;
    update_page(page);
}

static void replay_event(lv_event_t *event)
{
    recorder_page_t *page = event_page(event);
    aether_recorder_status_t status;
    if (!page || page->pending || page->stopping || !page->recorder ||
        aether_recorder_get_status(page->recorder, &status) != ESP_OK || status.active ||
        status.state != AETHER_RECORDER_FINISHED || !status.path[0] || !status.data_bytes) return;
    /* The next page owns its copy; no recorder-page pointer crosses navigation. */
    app_ui_t *ui = page->ui;
    app_open_music_path(ui, status.path);
    ui->page_back = app_open_recorder;
}

static void browse_event(lv_event_t *event)
{
    recorder_page_t *page = event_page(event);
    if (page && !page->pending && !page->gated) {
        app_ui_t *ui = page->ui;
        app_open_files_path(ui, AETHER_STORAGE_MOUNT_POINT "/Recordings");
        ui->page_back = app_open_recorder;
    }
}

static void input_event(lv_event_t *event)
{
    recorder_page_t *page = event_page(event);
    aether_recorder_status_t status;
    if (!page || page->pending || page->stopping || !page->recorder ||
        aether_recorder_get_status(page->recorder, &status) != ESP_OK || status.active) return;
    page->command_error = app_audio_system_set_input(lv_dropdown_get_selected(page->input) ?
                                                     APP_AUDIO_INPUT_USB : APP_AUDIO_INPUT_BOARD);
    update_page(page);
}

static void draw_waveform(lv_event_t *event)
{
    const recorder_page_t *page = lv_event_get_user_data(event);
    lv_area_t bounds;
    lv_obj_get_coords(page->waveform, &bounds);
    const int32_t width = (WAVE_SAMPLES - 1) * WAVE_STEP + WAVE_BAR_WIDTH;
    const int32_t origin = bounds.x1 + (lv_area_get_width(&bounds) - width) / 2;
    const int32_t center = bounds.y1 + 27;
    const size_t empty = WAVE_SAMPLES - page->wave_count;
    const size_t oldest = (page->wave_next + empty) % WAVE_SAMPLES;
    lv_draw_rect_dsc_t bar_style;
    lv_draw_rect_dsc_init(&bar_style);
    bar_style.radius = 2;
    lv_layer_t *layer = lv_event_get_layer(event);
    for (size_t i = 0; i < WAVE_SAMPLES; ++i) {
        const bool sampled = i >= empty;
        const unsigned level = sampled ? page->peaks[(oldest + i - empty) % WAVE_SAMPLES] : 0;
        const int32_t height = 2 + level * (WAVE_MAX_HEIGHT - 2) / 100U;
        bar_style.bg_color = lv_color_hex(!sampled ? 0x3A3B45 : i + 1 == WAVE_SAMPLES ? 0xFF6774 : 0xCB5965);
        const int32_t x = origin + (int32_t)i * WAVE_STEP;
        const lv_area_t bar = {x, center - height / 2, x + WAVE_BAR_WIDTH - 1, center + (height - 1) / 2};
        lv_draw_rect(layer, &bar_style, &bar);
    }
}

static void update_waveform(recorder_page_t *page, const aether_recorder_status_t *status, uint8_t level)
{
    if (status->state == AETHER_RECORDER_STARTING && page->wave_count) {
        page->wave_next = page->wave_count = 0;
        lv_obj_invalidate(page->waveform);
    } else if (status->state == AETHER_RECORDER_RECORDING &&
               (!page->wave_count || status->duration_ms != page->wave_duration)) {
        page->wave_duration = status->duration_ms;
        page->peaks[page->wave_next] = level;
        page->wave_next = (page->wave_next + 1) % WAVE_SAMPLES;
        if (page->wave_count < WAVE_SAMPLES) ++page->wave_count;
        lv_obj_invalidate(page->waveform);
    }
}

static void update_page(recorder_page_t *page)
{
    aether_recorder_status_t status = {0};
    const esp_err_t read_error = page->recorder ?
        aether_recorder_get_status(page->recorder, &status) : ESP_ERR_INVALID_STATE;
    const bool ready = read_error == ESP_OK;
    if (!status.active) page->stopping = false;
    const bool busy = page->pending || status.active || page->stopping;
    gate(page, busy);
    const bool mounted = recorder_storage_ready(page->ui);
    app_audio_system_snapshot_t audio;
    app_audio_system_get_snapshot(&audio);
    const uint8_t input_options = audio.usb_capture_connected ? 1 : audio.input == APP_AUDIO_INPUT_USB ? 2 : 0;
    if (page->input_options != input_options) {
        if (lv_dropdown_is_open(page->input)) lv_dropdown_close(page->input);
        lv_dropdown_set_options(page->input, input_options == 1 ? "板载麦克风\nUSB 麦克风" :
            input_options == 2 ? "板载麦克风\nUSB 麦克风（未连接）" : "板载麦克风");
        page->input_options = input_options;
    }
    const uint16_t selected = audio.input == APP_AUDIO_INPUT_USB ? 1 : 0;
    if (!lv_dropdown_is_open(page->input) && lv_dropdown_get_selected(page->input) != selected)
        lv_dropdown_set_selected(page->input, selected);
    const bool input_ready = audio.ready && audio.input == audio.applied_input &&
        (audio.input != APP_AUDIO_INPUT_USB || audio.usb_capture_connected);
    const bool can_start = ready && mounted && !busy && input_ready;
    enabled(page->input, audio.ready && !busy);
    enabled(page->start, can_start);
    enabled(page->pause, ready && !page->pending && !page->stopping &&
        (status.state == AETHER_RECORDER_RECORDING || status.state == AETHER_RECORDER_PAUSED));
    enabled(page->stop, ready && status.active && !page->pending && !page->stopping &&
        status.state != AETHER_RECORDER_SAVING);
    enabled(page->replay, ready && aether_media_fs_path_ready(page->ui->storage, status.path) && !busy && status.state == AETHER_RECORDER_FINISHED &&
        status.path[0] && status.data_bytes);
    enabled(page->browse, mounted && !busy);

    const char *state = "准备录音";
    if (!ready) state = "录音不可用";
    else if (page->pending || status.state == AETHER_RECORDER_STARTING) state = "正在准备录音…";
    else if (page->stopping || status.state == AETHER_RECORDER_SAVING) state = "正在保存…";
    else if (status.state == AETHER_RECORDER_RECORDING) state = "正在录音";
    else if (status.state == AETHER_RECORDER_PAUSED) state = "录音已暂停";
    else if (status.state == AETHER_RECORDER_FINISHED) state = "录音已保存";
    else if (status.state == AETHER_RECORDER_ERROR) state = "录音失败";
    app_ui_label_text(page->state, state);
    const lv_color_t state_color = lv_color_hex(status.state == AETHER_RECORDER_RECORDING ?
        0xFF9FAD : status.state == AETHER_RECORDER_ERROR ? 0xFFB38C : 0xEAF2FF);
    if (!lv_color_eq(lv_obj_get_style_text_color(page->state, 0), state_color))
        lv_obj_set_style_text_color(page->state, state_color, 0);
    app_ui_label_text(page->pause_label, status.state == AETHER_RECORDER_PAUSED ? "继续" : "暂停");
    app_ui_label_text(page->record_label, status.active ? "" : "开始录音");

    const uint64_t seconds = status.duration_ms / 1000U;
    if (seconds >= 3600U) app_ui_label_fmt(page->time, "%02llu:%02llu:%02llu",
        (unsigned long long)(seconds / 3600U), (unsigned long long)(seconds / 60U % 60U),
        (unsigned long long)(seconds % 60U));
    else app_ui_label_fmt(page->time, "%02llu:%02llu", (unsigned long long)(seconds / 60U),
                          (unsigned long long)(seconds % 60U));
    const uint32_t peak = status.state == AETHER_RECORDER_RECORDING ? status.peak : 0;
    const unsigned level = (unsigned)((peak > 32768U ? 32768U : peak) * 100U / 32768U);
    app_ui_label_fmt(page->level, "输入电平  %u%%", level);
    update_waveform(page, &status, (uint8_t)level);
    const lv_color_t dot = lv_color_hex(can_start || status.state == AETHER_RECORDER_RECORDING ? 0xF34B5D : 0x75535C);
    if (!lv_color_eq(lv_obj_get_style_bg_color(page->record_dot, 0), dot))
        lv_obj_set_style_bg_color(page->record_dot, dot, 0);

    const esp_err_t error = page->command_error != ESP_OK ? page->command_error : status.last_error;
    if (!ready) app_ui_label_text(page->message, "录音服务未就绪，请返回后重试");
    else if (error != ESP_OK) {
        if (status.last_error == error && status.detail[0]) app_ui_label_text(page->message, status.detail);
        else app_ui_label_fmt(page->message, "操作失败：%s\n请检查存储设备和麦克风连接", esp_err_to_name(error));
    }
    else if (!mounted) app_ui_label_text(page->message, "请插入 TF 卡");
    else if (page->pending) app_ui_label_text(page->message, "正在检查存储设备…");
    else if (audio.input == APP_AUDIO_INPUT_USB && !audio.usb_capture_connected)
        app_ui_label_text(page->message, "USB 麦克风已断开\n请重新连接或选择板载麦克风");
    else if (!input_ready) app_ui_label_text(page->message, "正在切换录音输入，请稍候…");
    else if (page->stopping || status.state == AETHER_RECORDER_SAVING)
        app_ui_label_text(page->message, "正在写入 WAV 文件头，请稍候…");
    else if (status.state == AETHER_RECORDER_FINISHED && status.path[0]) {
        const char *name = strrchr(status.path, '/');
        app_ui_label_text(page->message, name ? name + 1 : status.path);
    } else app_ui_label_text(page->message, "");
}

static void recorder_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    if (ui->page_cleanup == recorder_cleanup && ui->page_context) update_page(ui->page_context);
}

static void recorder_cleanup(app_ui_t *ui)
{
    recorder_page_t *page = ui->page_context;
    if (!page) return;
    aether_recorder_status_t status;
    if (page->recorder && aether_recorder_get_status(page->recorder, &status) == ESP_OK && status.active)
        aether_recorder_stop(page->recorder);
    /* The global recorder-active guard remains in force until async cleanup. */
    app_ui_media_control_gate(ui, false, false);
    lv_obj_remove_event_cb(page->waveform, draw_waveform);
    ui->page_context = NULL;
    free(page);
}

static lv_obj_t *button(recorder_page_t *page, const char *text, int x, int y, int width,
                        uint32_t color, lv_event_cb_t callback)
{
    lv_obj_t *object = lv_button_create(page->ui->content);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, 48);
    lv_obj_set_style_radius(object, 14, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x464652), LV_STATE_PRESSED);
    lv_obj_set_style_recolor(object, lv_color_hex(0x16151D), LV_STATE_DISABLED);
    lv_obj_set_style_recolor_opa(object, LV_OPA_60, LV_STATE_DISABLED);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, page->ui);
    app_ui_add_text(object, text, LV_ALIGN_CENTER, 0, 0, 0xF4F2F5, NULL);
    return object;
}

static void build_record_button(recorder_page_t *page)
{
    page->start = lv_button_create(page->ui->content);
    lv_obj_set_pos(page->start, 151, 180);
    lv_obj_set_size(page->start, 106, 96);
    lv_obj_set_style_pad_all(page->start, 0, 0);
    lv_obj_set_style_bg_opa(page->start, LV_OPA_TRANSP, 0);
    lv_obj_set_style_recolor_opa(page->start, LV_OPA_TRANSP, LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(page->start, 0, 0);
    lv_obj_add_event_cb(page->start, start_event, LV_EVENT_CLICKED, page->ui);

    lv_obj_t *ring = lv_obj_create(page->start);
    lv_obj_set_size(ring, 73, 73);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ring, lv_color_hex(0x27232C), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xD96D78), 0);

    page->record_dot = lv_obj_create(ring);
    lv_obj_set_size(page->record_dot, 53, 53);
    lv_obj_center(page->record_dot);
    lv_obj_remove_flag(page->record_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(page->record_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(page->record_dot, 0, 0);
    page->record_label = app_ui_add_text(page->start, "开始录音", LV_ALIGN_BOTTOM_MID,
        0, -2, 0xCBC6CC, NULL);
}

void app_open_recorder(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "录音机");
    recorder_page_t *page = calloc(1, sizeof(*page));
    if (!page) { app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFFAAAA, NULL); return; }
    page->ui = ui;
    page->generation = ui->page_generation;
    page->recorder = aether_recorder_get_default();
    ui->page_context = page;
    ui->page_cleanup = recorder_cleanup;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x181820), 0);
    page->state = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 12, 9, 0xEAE4EA, NULL);
    lv_obj_set_size(page->state, 174, 22);
    lv_label_set_long_mode(page->state, LV_LABEL_LONG_DOT);
    page->input = lv_dropdown_create(content);
    lv_obj_set_pos(page->input, 207, 1);
    lv_obj_set_size(page->input, 189, 33);
    lv_dropdown_set_options(page->input, "板载麦克风");
    lv_obj_set_style_text_font(page->input, app_ui_chinese_font(), 0);
    lv_obj_set_style_bg_color(page->input, lv_color_hex(0x2D2B34), 0);
    lv_obj_set_style_text_color(page->input, lv_color_hex(0xEAE4EA), 0);
    lv_obj_set_style_recolor(page->input, lv_color_hex(0x16151D), LV_STATE_DISABLED);
    lv_obj_set_style_recolor_opa(page->input, LV_OPA_60, LV_STATE_DISABLED);
    lv_obj_set_style_border_width(page->input, 0, 0);
    lv_obj_set_style_radius(page->input, 10, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(page->input), app_ui_chinese_font(), 0);
    lv_obj_set_style_bg_color(lv_dropdown_get_list(page->input), lv_color_hex(0x282630), 0);
    lv_obj_set_style_text_color(lv_dropdown_get_list(page->input), lv_color_hex(0xEAF2FF), 0);
    lv_obj_add_event_cb(page->input, input_event, LV_EVENT_VALUE_CHANGED, ui);
    page->time = app_ui_add_text(content, "00:00", LV_ALIGN_TOP_MID, 0, 41,
                                  0xF9F3F7, &lv_font_montserrat_48);
    page->waveform = lv_obj_create(content);
    lv_obj_set_pos(page->waveform, 12, 106);
    lv_obj_set_size(page->waveform, 384, 69);
    lv_obj_remove_flag(page->waveform, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(page->waveform, 0, 0);
    lv_obj_set_style_radius(page->waveform, 15, 0);
    lv_obj_set_style_border_width(page->waveform, 0, 0);
    lv_obj_set_style_bg_color(page->waveform, lv_color_hex(0x24232C), 0);
    lv_obj_add_event_cb(page->waveform, draw_waveform, LV_EVENT_DRAW_POST, page);
    page->level = app_ui_add_text(page->waveform, "输入电平  0%", LV_ALIGN_BOTTOM_MID, 0, -3, 0xB9AEB9, NULL);

    build_record_button(page);
    page->pause = button(page, "暂停", 15, 194, 111, 0x30303A, pause_event);
    page->pause_label = lv_obj_get_child(page->pause, 0);
    page->stop = button(page, "停止保存", 282, 194, 111, 0x30303A, stop_event);
    page->message = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 275, 0xC0ADB9, NULL);
    lv_obj_set_size(page->message, 384, 36);
    lv_obj_set_style_text_align(page->message, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(page->message, LV_LABEL_LONG_WRAP);
    page->replay = button(page, "回放录音", 12, 308, 186, 0x2D2B34, replay_event);
    page->browse = button(page, "录音文件", 210, 308, 186, 0x2D2B34, browse_event);
    lv_obj_set_height(page->replay, 44);
    lv_obj_set_height(page->browse, 44);
    app_ui_set_timer(ui, recorder_timer, 100);
    update_page(page);
}
