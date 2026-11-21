#include "app_ui_internal.h"
#include "aether_recorder.h"
#include "app_audio_system.h"
#include "aether_media_fs.h"

#include <stdlib.h>
#include <stdio.h>

typedef struct {
    app_ui_t *ui;
    aether_recorder_handle_t recorder;
    uint32_t generation;
    bool pending, stopping, gated;
    uint8_t input_options;
    esp_err_t command_error;
    lv_obj_t *state, *time, *level, *meter, *message, *path;
    lv_obj_t *start, *pause, *pause_label, *stop, *replay, *browse;
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
    app_open_music_path(page->ui, status.path);
}

static void browse_event(lv_event_t *event)
{
    recorder_page_t *page = event_page(event);
    if (page && !page->pending && !page->gated) {
        app_open_files_path(page->ui, AETHER_STORAGE_MOUNT_POINT "/Recordings");
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
    if (!lv_dropdown_is_open(page->input)) lv_dropdown_set_selected(page->input,
        audio.input == APP_AUDIO_INPUT_USB ? 1 : 0);
    const bool input_ready = audio.ready && audio.input == audio.applied_input &&
        (audio.input != APP_AUDIO_INPUT_USB || audio.usb_capture_connected);
    enabled(page->input, audio.ready && !busy);
    enabled(page->start, ready && mounted && !busy && input_ready);
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
    lv_obj_set_style_text_color(page->state, lv_color_hex(status.state == AETHER_RECORDER_RECORDING ?
        0xFF9FAD : status.state == AETHER_RECORDER_ERROR ? 0xFFB38C : 0xEAF2FF), 0);
    app_ui_label_text(page->pause_label, status.state == AETHER_RECORDER_PAUSED ? "继续" : "暂停");

    const uint64_t seconds = status.duration_ms / 1000U;
    if (seconds >= 3600U) app_ui_label_fmt(page->time, "%02llu:%02llu:%02llu",
        (unsigned long long)(seconds / 3600U), (unsigned long long)(seconds / 60U % 60U),
        (unsigned long long)(seconds % 60U));
    else app_ui_label_fmt(page->time, "%02llu:%02llu", (unsigned long long)(seconds / 60U),
                          (unsigned long long)(seconds % 60U));
    const uint32_t peak = status.state == AETHER_RECORDER_RECORDING ? status.peak : 0;
    const unsigned level = (unsigned)((peak > 32768U ? 32768U : peak) * 100U / 32768U);
    app_ui_label_fmt(page->level, "输入电平  %u%%", level);
    lv_bar_set_value(page->meter, (int32_t)level, LV_ANIM_OFF);
    if (status.path[0]) app_ui_label_text(page->path, status.path);
    else app_ui_label_text(page->path, AETHER_STORAGE_MOUNT_POINT "/Recordings");

    const esp_err_t error = page->command_error != ESP_OK ? page->command_error : status.last_error;
    if (!ready) app_ui_label_text(page->message, "录音服务未就绪，请返回后重试");
    else if (error != ESP_OK) {
        if (status.last_error == error && status.detail[0]) app_ui_label_text(page->message, status.detail);
        else app_ui_label_fmt(page->message, "操作失败：%s\n请检查存储设备和麦克风连接", esp_err_to_name(error));
    }
    else if (!mounted) app_ui_label_text(page->message, "请插入 TF 卡；录音保存在 Recordings 文件夹");
    else if (page->pending) app_ui_label_text(page->message, "正在停止其他媒体并检查存储设备…");
    else if (audio.input == APP_AUDIO_INPUT_USB && !audio.usb_capture_connected)
        app_ui_label_text(page->message, "USB 麦克风已断开\n请重新连接或选择板载麦克风");
    else if (!input_ready) app_ui_label_text(page->message, "正在切换录音输入，请稍候…");
    else if (page->stopping || status.state == AETHER_RECORDER_SAVING)
        app_ui_label_text(page->message, "正在写入 WAV 文件头，请稍候…");
    else if (status.state == AETHER_RECORDER_FINISHED) app_ui_label_fmt(page->message,
        "已保存 %llu.%llu MiB · 可回放或浏览文件", (unsigned long long)(status.data_bytes / 1048576U),
        (unsigned long long)(status.data_bytes % 1048576U * 10U / 1048576U));
    else app_ui_label_text(page->message, "离开页面会自动停止并保存\n暂停期间不计入录音时长");
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
    lv_obj_set_style_bg_color(object, lv_color_hex(0x435777), LV_STATE_PRESSED);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, page->ui);
    app_ui_add_text(object, text, LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, NULL);
    return object;
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
    page->state = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 0, 0xEAF2FF, app_ui_chinese_font_size(24));
    lv_obj_set_size(page->state, 202, 28);
    page->input = lv_dropdown_create(content);
    lv_obj_set_pos(page->input, 214, 0);
    lv_obj_set_size(page->input, 194, 34);
    lv_dropdown_set_options(page->input, "板载麦克风");
    lv_obj_set_style_text_font(page->input, app_ui_chinese_font(), 0);
    lv_obj_set_style_bg_color(page->input, lv_color_hex(0x263C58), 0);
    lv_obj_set_style_text_color(page->input, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_border_width(page->input, 0, 0);
    lv_obj_set_style_radius(page->input, 10, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(page->input), app_ui_chinese_font(), 0);
    lv_obj_set_style_bg_color(lv_dropdown_get_list(page->input), lv_color_hex(0x1A2A42), 0);
    lv_obj_set_style_text_color(lv_dropdown_get_list(page->input), lv_color_hex(0xEAF2FF), 0);
    lv_obj_add_event_cb(page->input, input_event, LV_EVENT_VALUE_CHANGED, ui);
    app_ui_add_text(content, "PCM WAV · 48 kHz · 16位 · 单声道", LV_ALIGN_TOP_LEFT,
                    0, 42, 0x91A8C7, NULL);
    page->time = app_ui_add_text(content, "00:00", LV_ALIGN_TOP_MID, 0, 72,
                                  0xFFE2E8, &lv_font_montserrat_28);
    page->level = app_ui_add_text(content, "输入电平  0%", LV_ALIGN_TOP_LEFT, 0, 108, 0xAEC3DF, NULL);
    page->meter = lv_bar_create(content);
    lv_obj_set_pos(page->meter, 0, 136);
    lv_obj_set_size(page->meter, 408, 10);
    lv_bar_set_range(page->meter, 0, 100);
    lv_obj_set_style_bg_color(page->meter, lv_color_hex(0x24354E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(page->meter, lv_color_hex(0x7EDDB6), LV_PART_INDICATOR);
    page->start = button(page, "开始录音", 0, 164, 128, 0xB94661, start_event);
    page->pause = button(page, "暂停", 140, 164, 128, 0x284465, pause_event);
    page->pause_label = lv_obj_get_child(page->pause, 0);
    page->stop = button(page, "停止保存", 280, 164, 128, 0x284465, stop_event);
    page->message = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 224, 0xAEC3DF, NULL);
    lv_obj_set_size(page->message, 408, 40);
    lv_label_set_long_mode(page->message, LV_LABEL_LONG_WRAP);
    page->path = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 272, 0x7FA2CC, NULL);
    lv_obj_set_size(page->path, 408, 20);
    lv_label_set_long_mode(page->path, LV_LABEL_LONG_DOT);
    page->replay = button(page, "回放录音", 0, 306, 198, 0x263C58, replay_event);
    page->browse = button(page, "录音文件", 210, 306, 198, 0x263C58, browse_event);
    app_ui_set_timer(ui, recorder_timer, 100);
    update_page(page);
}
