#include "app_ui_internal.h"
#include "app_audio_system.h"
#include "aether_media_fs.h"
#include "ui/app_scroll_list.h"
#include "app_text.h"
#include "app_usb_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

enum { MUSIC_EQ_GAIN_ROW = AETHER_AUDIO_EQ_BANDS, MUSIC_EQ_ROWS = MUSIC_EQ_GAIN_ROW + 1 };

typedef struct {
    app_ui_t *ui;
    app_io_entry_t *entries;
    size_t count;
    app_scroll_list_t tracks;
    bool loading;
    bool seeking;
    bool was_mounted;
    unsigned catalog_retries;
    int64_t retry_at_us;
    char requested_path[192];
    char library_dir[192];
    lv_obj_t *title, *status, *time, *progress, *volume, *volume_label;
    lv_obj_t *play_label, *repeat_label, *library, *library_count;
    lv_obj_t *eq_panel, *eq_toggle_label, *eq_status;
    lv_obj_t *eq_sliders[MUSIC_EQ_ROWS], *eq_levels[MUSIC_EQ_ROWS];
    aether_audio_eq_t eq;
} music_page_t;

static const char *filename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool music_storage_ready(app_ui_t *ui, const char *path)
{
    return !app_io_usb_storage_blocked(ui->io) &&
        aether_media_fs_path_ready(ui->storage, path && path[0] ? path : aether_media_fs_default_root(ui->storage));
}

static music_page_t *page(lv_event_t *e)
{
    app_ui_t *ui = lv_event_get_user_data(e);
    return ui ? ui->page_context : NULL;
}

static lv_obj_t *button(music_page_t *p, lv_obj_t *parent, const char *text,
                        int x, int y, int width, int height, lv_event_cb_t callback)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, width, height);
    lv_obj_set_style_radius(b, 13, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x374A66), 0);
    lv_obj_add_event_cb(b, callback, LV_EVENT_CLICKED, p->ui);
    app_ui_add_text(b, text, LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, app_ui_chinese_font());
    return b;
}

static bool select_path(music_page_t *p, const char *path)
{
    if (p->loading || !p->ui->audio_player || !path || !path[0]) return false;
    size_t selected = p->count;
    for (size_t i = 0; i < p->count; ++i) if (!strcmp(path, p->entries[i].path)) { selected = i; break; }
    if (selected == p->count) {
        const char *paths[] = {path};
        const esp_err_t error = aether_audio_player_set_playlist(p->ui->audio_player, paths, 1);
        if (error != ESP_OK) {
            app_ui_label_fmt(p->status, "暂时无法选择歌曲：%s", esp_err_to_name(error));
            return false;
        }
        /* A one-song fallback replaces the player's list. Discard matching UI
         * indices from the previous catalog before any later row is clicked. */
        free(p->entries); p->entries = NULL; p->count = 0;
        app_scroll_list_set_count(&p->tracks, 0);
        app_ui_label_text(p->library_count, "曲库为空");
        selected = 0;
    }
    const esp_err_t err = aether_audio_player_select(p->ui->audio_player, selected);
    if (err != ESP_OK) { app_ui_label_fmt(p->status, "选歌失败：%s", esp_err_to_name(err)); return false; }
    app_usb_input_modal_close(p->library);
    lv_obj_add_flag(p->library, LV_OBJ_FLAG_HIDDEN);
    app_ui_label_text(p->title, filename(path));
    app_ui_label_text(p->status, "正在切换歌曲…");
    return true;
}

static void select_track(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (!p || p->loading) return;
    const size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (index < p->count) select_path(p, p->entries[index].path);
}

static lv_obj_t *create_track_row(lv_obj_t *parent, void *context)
{
    music_page_t *p = context;
    lv_obj_t *row = button(p, parent, "", 0, 0, 382, 43, select_track);
    lv_obj_t *label = lv_obj_get_child(row, 0);
    lv_obj_set_width(label, 350);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    return row;
}

static void bind_track_row(lv_obj_t *row, size_t index, void *context)
{
    const music_page_t *p = context;
    char name[sizeof(p->entries[index].path)];
    strlcpy(name, filename(p->entries[index].path), sizeof(name));
    app_text_sanitize_utf8(name, strlen(name));
    app_ui_label_text(lv_obj_get_child(row, 0), name);
}

static void render_library(music_page_t *p)
{
    app_scroll_list_set_count(&p->tracks, p->count);
    if (p->count) app_ui_label_fmt(p->library_count, "%u 首", (unsigned)p->count);
    else app_ui_label_text(p->library_count, "曲库为空");
}

static void list_close(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (p) { app_usb_input_modal_close(p->library); lv_obj_add_flag(p->library, LV_OBJ_FLAG_HIDDEN); }
}

static void list_show(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (p) {
        lv_obj_remove_flag(p->library, LV_OBJ_FLAG_HIDDEN);
        app_usb_input_modal_open(p->library, NULL);
    }
}

static void eq_level(music_page_t *p, unsigned row, int value)
{
    app_ui_label_fmt(p->eq_levels[row], value > 0 ? "+%d dB" : "%d dB", value);
}

static void eq_sync(music_page_t *p)
{
    for (unsigned row = 0; row < MUSIC_EQ_ROWS; ++row) {
        const int value = row == MUSIC_EQ_GAIN_ROW ? p->eq.gain_db : p->eq.bands_db[row];
        lv_slider_set_value(p->eq_sliders[row], value, LV_ANIM_OFF);
        eq_level(p, row, value);
    }
    app_ui_label_text(p->eq_toggle_label, p->eq.enabled ? "对比原声" : "启用 EQ");
}

static bool eq_apply(music_page_t *p, const aether_audio_eq_t *settings)
{
    const esp_err_t error = aether_audio_player_set_eq(p->ui->audio_player, settings);
    if (error != ESP_OK) {
        app_ui_label_fmt(p->eq_status, "调整失败：%s", esp_err_to_name(error));
        eq_sync(p);
        return false;
    }
    p->eq = *settings;
    app_ui_label_text(p->eq_status, p->eq.enabled ? "EQ 已启用" : "当前为原声");
    app_ui_label_text(p->eq_toggle_label, p->eq.enabled ? "对比原声" : "启用 EQ");
    return true;
}

static void eq_change(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (!p || !p->ui->audio_player) return;
    lv_obj_t *slider = lv_event_get_target_obj(e);
    const unsigned row = (unsigned)(uintptr_t)lv_obj_get_user_data(slider);
    if (row >= MUSIC_EQ_ROWS) return;
    const int value = lv_slider_get_value(slider);
    aether_audio_eq_t settings = p->eq;
    settings.enabled = true;
    if (row == MUSIC_EQ_GAIN_ROW) settings.gain_db = (uint8_t)value;
    else settings.bands_db[row] = (int8_t)value;
    if (eq_apply(p, &settings)) eq_level(p, row, value);
}

static void eq_toggle(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (!p || !p->ui->audio_player) return;
    aether_audio_eq_t settings = p->eq;
    settings.enabled = !settings.enabled;
    eq_apply(p, &settings);
}

static void eq_reset(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (!p || !p->ui->audio_player) return;
    const aether_audio_eq_t settings = {0};
    if (eq_apply(p, &settings)) {
        eq_sync(p);
        app_ui_label_text(p->eq_status, "已恢复默认值");
    }
}

static void eq_close(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (!p) return;
    app_usb_input_modal_close(p->eq_panel);
    lv_obj_add_flag(p->eq_panel, LV_OBJ_FLAG_HIDDEN);
}

static void eq_show(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (!p || !p->ui->audio_player) return;
    aether_audio_player_status_t s;
    const esp_err_t error = aether_audio_player_get_status(p->ui->audio_player, &s);
    if (error != ESP_OK) {
        app_ui_label_fmt(p->status, "读取均衡器失败：%s", esp_err_to_name(error));
        return;
    }
    p->eq = s.eq;
    eq_sync(p);
    app_ui_label_text(p->eq_status, p->eq.enabled ? "EQ 已启用" : "拖动滑块即可启用 EQ");
    lv_obj_remove_flag(p->eq_panel, LV_OBJ_FLAG_HIDDEN);
    app_usb_input_modal_open(p->eq_panel, NULL);
}

static void build_eq_panel(music_page_t *p, lv_obj_t *parent)
{
    static const char *const labels[MUSIC_EQ_ROWS] = {"100 Hz", "300 Hz", "1 kHz", "3 kHz", "10 kHz", "音乐增益"};
    p->eq_panel = lv_obj_create(parent);
    lv_obj_set_pos(p->eq_panel, 0, 0);
    lv_obj_set_size(p->eq_panel, 408, 354);
    lv_obj_set_style_pad_all(p->eq_panel, 8, 0);
    lv_obj_set_style_bg_color(p->eq_panel, lv_color_hex(0x10192A), 0);
    lv_obj_set_style_bg_opa(p->eq_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p->eq_panel, 0, 0);
    lv_obj_remove_flag(p->eq_panel, LV_OBJ_FLAG_SCROLLABLE);
    button(p, p->eq_panel, "返回", 0, 0, 66, 36, eq_close);
    app_ui_add_text(p->eq_panel, "均衡器", LV_ALIGN_TOP_LEFT, 79, 9, 0xF6C175, app_ui_chinese_font());
    lv_obj_t *toggle = button(p, p->eq_panel, "启用 EQ", 151, 0, 111, 36, eq_toggle);
    p->eq_toggle_label = lv_obj_get_child(toggle, 0);
    button(p, p->eq_panel, "恢复默认", 274, 0, 118, 36, eq_reset);

    for (unsigned row = 0; row < MUSIC_EQ_ROWS; ++row) {
        const bool gain = row == MUSIC_EQ_GAIN_ROW;
        const int y = 53 + (int)row * 43;
        app_ui_add_text(p->eq_panel, labels[row], LV_ALIGN_TOP_LEFT, 3, y, 0xD9E8FF, app_ui_chinese_font());
        lv_obj_t *slider = lv_slider_create(p->eq_panel);
        p->eq_sliders[row] = slider;
        lv_obj_set_pos(slider, 92, y + 3);
        lv_obj_set_size(slider, 230, 14);
        lv_obj_set_ext_click_area(slider, 10);
        lv_slider_set_range(slider, gain ? 0 : -AETHER_AUDIO_EQ_MAX_DB,
                           gain ? AETHER_AUDIO_EQ_MAX_GAIN_DB : AETHER_AUDIO_EQ_MAX_DB);
        if (!gain) lv_slider_set_mode(slider, LV_SLIDER_MODE_SYMMETRICAL);
        lv_obj_set_style_bg_color(slider, lv_color_hex(0x2B3C57), LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, lv_color_hex(0x699AEB), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, lv_color_hex(0xDAE8FF), LV_PART_KNOB);
        lv_obj_set_user_data(slider, (void *)(uintptr_t)row);
        lv_obj_add_event_cb(slider, eq_change, LV_EVENT_VALUE_CHANGED, p->ui);
        p->eq_levels[row] = app_ui_add_text(p->eq_panel, "0 dB", LV_ALIGN_TOP_RIGHT, 0, y,
                                            0x97B9F4, app_ui_chinese_font());
    }
    p->eq_status = app_ui_add_text(p->eq_panel, "", LV_ALIGN_TOP_LEFT, 3, 315, 0x91A8C9, app_ui_chinese_font());
    lv_obj_set_width(p->eq_status, 389);
    lv_label_set_long_mode(p->eq_status, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(p->eq_panel, LV_OBJ_FLAG_HIDDEN);
}

static void retry_catalog_later(music_page_t *p)
{
    if (p->catalog_retries >= 3U) return;
    p->retry_at_us = esp_timer_get_time() + (250000LL << p->catalog_retries);
    ++p->catalog_retries;
}

static void loaded(app_ui_t *ui, const app_io_result_t *result)
{
    music_page_t *p = ui->page_context;
    if (!p) return;
    p->loading = false;
    if (result->playlist_prepared) {
        free(p->entries); p->entries = NULL; p->count = 0;
    }
    if (result->error != ESP_OK || !result->count) {
        if (result->playlist_prepared) render_library(p);
        if (result->error == ESP_ERR_INVALID_STATE && p->was_mounted && music_storage_ready(ui, p->library_dir))
            retry_catalog_later(p);
        app_ui_label_text(p->status, result->error == ESP_OK || result->error == ESP_ERR_NOT_FOUND ?
                          "未找到 WAV/MP3：请放入 Music 文件夹或根目录" : "曲库读取失败，请检查 TF 卡后刷新");
        /* Only a successful scan proves that app_io completed the NES/video
         * stop barriers. NOT_FOUND can also come from storage acquisition;
         * no error result is permission to start another audio owner. */
        if (result->error == ESP_OK && p->requested_path[0] && music_storage_ready(ui, p->requested_path)) {
            if (select_path(p, p->requested_path)) p->requested_path[0] = '\0';
        }
        return;
    }
    if (!result->playlist_prepared) {
        retry_catalog_later(p);
        app_ui_label_text(p->status, "曲库尚未准备好，稍后重试…");
        return;
    }
    app_io_entry_t *entries = heap_caps_malloc(result->count * sizeof(*entries), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) {
        render_library(p);
        app_ui_label_text(p->status, "曲库内存不足");
        return;
    }
    memcpy(entries, result->entries, result->count * sizeof(*entries));
    /* The IO worker already allocated/copied the backend playlist. Never
     * interpret old UI indices after that worker list changes. */
    free(p->entries); p->entries = entries;
    p->count = result->count;
    p->retry_at_us = 0;
    p->catalog_retries = 0;
    if (p->requested_path[0]) {
        if (select_path(p, p->requested_path)) p->requested_path[0] = '\0';
    } else {
        aether_audio_player_status_t status;
        aether_audio_player_get_status(ui->audio_player, &status);
        if (!status.path[0]) app_ui_label_text(p->title, filename(p->entries[0].path));
    }
    if (result->truncated) app_ui_label_text(p->status, "曲库超过4096首，请分目录存放");
    render_library(p);
}

static void refresh_catalog(music_page_t *p)
{
    app_io_request_t request = {.operation = APP_IO_CATALOG, .filter = APP_IO_MUSIC, .limit = 4096};
    /* Opening through the file browser scans that directory as its playlist. */
    strlcpy(request.path, p->library_dir, sizeof(request.path));
    p->loading = true;
    p->retry_at_us = 0;
    app_ui_label_text(p->status, "正在扫描曲库…");
    app_ui_request_io_ex(p->ui, &request, loaded);
}

static void refresh_event(lv_event_t *e)
{
    music_page_t *p = page(e);
    /* One preparation owns ui->io_complete. A held key/repeated tap must not
     * overwrite it or queue repeated filesystem scans during playback. */
    if (p && !p->loading) { p->catalog_retries = 0; refresh_catalog(p); }
}

static void play_event(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (!p || !p->ui->audio_player) return;
    aether_audio_player_status_t s;
    aether_audio_player_get_status(p->ui->audio_player, &s);
    if (s.active) {
        aether_audio_player_set_paused(p->ui->audio_player, s.state != AETHER_AUDIO_PAUSED);
    } else if (p->loading) return;
    else if (s.path[0]) select_path(p, s.path);
    else if (p->count) select_path(p, p->entries[0].path);
}

static void stop_event(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (p && p->ui->audio_player) aether_audio_player_stop(p->ui->audio_player);
}

static void previous_event(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (p && !p->loading && p->ui->audio_player) aether_audio_player_next(p->ui->audio_player, -1);
}

static void next_event(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (p && !p->loading && p->ui->audio_player) aether_audio_player_next(p->ui->audio_player, 1);
}

static void repeat_event(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (!p || !p->ui->audio_player) return;
    aether_audio_player_status_t s;
    aether_audio_player_get_status(p->ui->audio_player, &s);
    aether_audio_player_set_repeat(p->ui->audio_player, (s.repeat + 1) % 3);
}

static void volume_event(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (!p) return;
    const uint8_t value = (uint8_t)lv_slider_get_value(p->volume);
    const esp_err_t error = app_audio_system_set_volume(value, lv_event_get_code(e) == LV_EVENT_RELEASED);
    app_ui_label_fmt(p->volume_label, "系统音量 %u%%", value);
    if (error != ESP_OK) app_ui_label_fmt(p->status, "调整音量失败：%s", esp_err_to_name(error));
}

static void refresh_volume(music_page_t *p)
{
    app_audio_system_snapshot_t audio;
    app_audio_system_get_snapshot(&audio);
    if (!lv_obj_has_state(p->volume, LV_STATE_PRESSED))
        lv_slider_set_value(p->volume, audio.master_volume, LV_ANIM_OFF);
    app_ui_label_fmt(p->volume_label, "系统音量 %u%%", (unsigned)lv_slider_get_value(p->volume));
    if (audio.ready) lv_obj_remove_state(p->volume, LV_STATE_DISABLED);
    else lv_obj_add_state(p->volume, LV_STATE_DISABLED);
}

static void seek_event(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (!p || !p->ui->audio_player) return;
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_INDEV_RESET) {
        p->seeking = false;
        return;
    }
    if (code == LV_EVENT_PRESSED) p->seeking = true;
    if (code == LV_EVENT_RELEASED && p->seeking) {
        aether_audio_player_status_t s;
        aether_audio_player_get_status(p->ui->audio_player, &s);
        if (s.active) aether_audio_player_seek(p->ui->audio_player,
            (uint32_t)((uint64_t)s.duration_ms * lv_slider_get_value(p->progress) / 1000));
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET)
        p->seeking = false;
}

static void music_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    music_page_t *p = ui->page_context;
    if (!p) return;
    refresh_volume(p);
    if (!ui->audio_player) return;
    aether_audio_player_status_t s = {0};
    aether_audio_player_get_status(ui->audio_player, &s);
    const bool mounted = music_storage_ready(ui, s.active ? s.path : p->library_dir);
    if (!mounted) {
        p->was_mounted = false;
        app_ui_label_text(p->status, "存储设备未就绪，请插卡或接入 USB 存储后刷新");
        return;
    }
    if (!p->was_mounted && !p->loading) {
        p->was_mounted = true;
        p->catalog_retries = 0;
        refresh_catalog(p);
    }
    if (p->retry_at_us && !p->loading && esp_timer_get_time() >= p->retry_at_us) refresh_catalog(p);
    if (s.path[0]) app_ui_label_text(p->title, filename(s.path));
    const uint32_t seconds = s.position_ms / 1000, total = s.duration_ms / 1000;
    app_ui_label_fmt(p->time, "%02lu:%02lu / %02lu:%02lu  ·  %u/%u",
        (unsigned long)(seconds / 60), (unsigned long)(seconds % 60),
        (unsigned long)(total / 60), (unsigned long)(total % 60),
        s.track_count ? s.track_index + 1 : 0, s.track_count);
    if (!p->seeking) lv_slider_set_value(p->progress, s.duration_ms ?
        (int32_t)((uint64_t)s.position_ms * 1000 / s.duration_ms) : 0, LV_ANIM_OFF);
    app_ui_label_text(p->play_label, s.state == AETHER_AUDIO_PLAYING ? "暂停" :
                      s.state == AETHER_AUDIO_PAUSED ? "继续" : "播放");
    static const char *const modes[] = {"列表循环", "单曲循环", "随机播放"};
    app_ui_label_text(p->repeat_label, modes[s.repeat <= AETHER_AUDIO_SHUFFLE ? s.repeat : 0]);
    if (!p->loading) {
        if (s.state == AETHER_AUDIO_ERROR) app_ui_label_fmt(p->status, "播放失败：%s", esp_err_to_name(s.last_error));
        else if (s.state == AETHER_AUDIO_STARTING) app_ui_label_text(p->status, "正在打开音乐…");
        else if (s.state == AETHER_AUDIO_PLAYING || s.state == AETHER_AUDIO_PAUSED) {
            app_ui_label_fmt(p->status, "%s  %lu Hz · %s · %s",
                s.state == AETHER_AUDIO_PAUSED ? "已暂停" : "播放中",
                (unsigned long)s.sample_rate, s.channels == 1 ? "单声道" : "立体声", s.aac ? "AAC-LC" : s.mp3 ? "MP3" : "PCM16");
        } else app_ui_label_text(p->status, p->count ? "点击播放或从曲库选歌" : "未找到音乐，请刷新曲库");
        if (s.settings_error != ESP_OK) app_ui_label_text(p->status, "播放器设置未能保存，重启后可能恢复旧设置");
    }
}

static void music_cleanup(app_ui_t *ui)
{
    music_page_t *p = ui->page_context;
    if (!p) return;
    app_usb_input_modal_close(p->eq_panel);
    app_scroll_list_deinit(&p->tracks);
    free(p->entries);
    free(p);
    ui->page_context = NULL;
    /* Leaving music stops the stream and releases decoder/codec/file/playlist
     * off LVGL. app_ui_new_screen already invalidated old preparation results. */
    (void)app_io_release_media(ui->io);
}

void app_open_music_path(app_ui_t *ui, const char *path)
{
    char requested[192] = {0};
    if (path) strlcpy(requested, path, sizeof(requested));
    lv_obj_t *content = app_ui_begin_page(ui, "音乐播放器");
    music_page_t *p = calloc(1, sizeof(*p));
    if (!p) { app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, app_ui_chinese_font()); return; }
    p->ui = ui;
    p->was_mounted = music_storage_ready(ui, requested);
    strlcpy(p->requested_path, requested, sizeof(p->requested_path));
    strlcpy(p->library_dir, requested, sizeof(p->library_dir));
    char *slash = strrchr(p->library_dir, '/');
    if (slash) *slash = '\0';
    else p->library_dir[0] = '\0';
    ui->page_context = p;
    ui->page_cleanup = music_cleanup;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    p->title = app_ui_add_text(content, "请选择音乐", LV_ALIGN_TOP_LEFT, 4, 0, 0xF6C175, app_ui_chinese_font_size(24));
    lv_obj_set_width(p->title, 396);
    lv_label_set_long_mode(p->title, LV_LABEL_LONG_DOT);
    p->status = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 4, 36, 0xACC0DB, app_ui_chinese_font());
    lv_obj_set_width(p->status, 396);
    lv_label_set_long_mode(p->status, LV_LABEL_LONG_DOT);
    p->progress = lv_slider_create(content);
    lv_obj_set_pos(p->progress, 10, 77); lv_obj_set_size(p->progress, 382, 12);
    lv_slider_set_range(p->progress, 0, 1000);
    lv_obj_add_event_cb(p->progress, seek_event, LV_EVENT_ALL, ui);
    p->time = app_ui_add_text(content, "00:00 / 00:00", LV_ALIGN_TOP_LEFT, 4, 98, 0x9CB4D4, app_ui_chinese_font());
    button(p, content, "上一首", 0, 134, 96, 46, previous_event);
    lv_obj_t *play = button(p, content, "播放", 104, 134, 96, 46, play_event);
    p->play_label = lv_obj_get_child(play, 0);
    button(p, content, "下一首", 208, 134, 96, 46, next_event);
    button(p, content, "停止", 312, 134, 92, 46, stop_event);
    p->volume_label = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 202, 0xD9E8FF, app_ui_chinese_font());
    p->volume = lv_slider_create(content);
    lv_obj_set_pos(p->volume, 115, 208); lv_obj_set_size(p->volume, 278, 18);
    lv_obj_set_ext_click_area(p->volume, 14);
    lv_slider_set_range(p->volume, 0, 100);
    lv_obj_add_event_cb(p->volume, volume_event, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(p->volume, volume_event, LV_EVENT_RELEASED, ui);
    refresh_volume(p);
    lv_obj_t *repeat = button(p, content, "列表循环", 0, 252, 112, 42, repeat_event);
    p->repeat_label = lv_obj_get_child(repeat, 0);
    button(p, content, "曲库", 121, 252, 81, 42, list_show);
    button(p, content, "刷新", 211, 252, 81, 42, refresh_event);
    lv_obj_t *eq = button(p, content, "均衡器", 301, 252, 103, 42, eq_show);
    if (!ui->audio_player) lv_obj_add_state(eq, LV_STATE_DISABLED);
    p->library = lv_obj_create(content);
    lv_obj_set_pos(p->library, 0, 0); lv_obj_set_size(p->library, 408, 354);
    lv_obj_set_style_pad_all(p->library, 8, 0);
    lv_obj_set_style_bg_color(p->library, lv_color_hex(0x10192A), 0);
    lv_obj_set_style_border_width(p->library, 0, 0);
    lv_obj_remove_flag(p->library, LV_OBJ_FLAG_SCROLLABLE);
    button(p, p->library, "返回", 0, 0, 68, 36, list_close);
    p->library_count = app_ui_add_text(p->library, "曲库", LV_ALIGN_TOP_RIGHT, -4, 8, 0xF6C175, app_ui_chinese_font());
    lv_obj_t *rows = lv_obj_create(p->library);
    lv_obj_set_pos(rows, 0, 44);
    lv_obj_set_size(rows, 392, 294);
    lv_obj_set_style_pad_all(rows, 0, 0);
    lv_obj_set_style_border_width(rows, 0, 0);
    lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
    app_scroll_list_init(&p->tracks, rows, 48, create_track_row, bind_track_row, p);
    lv_obj_add_flag(p->library, LV_OBJ_FLAG_HIDDEN);
    build_eq_panel(p, content);
    app_ui_set_timer(ui, music_timer, 120);
    if (!ui->audio_player) app_ui_label_text(p->status, "音频驱动未就绪，请检查 AC101S 供电和总线");
    else refresh_catalog(p);
}

void app_open_music(app_ui_t *ui)
{
    app_open_music_path(ui, NULL);
}
