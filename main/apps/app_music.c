#include "app_ui_internal.h"
#include "app_audio_system.h"
#include "app_lyrics.h"
#include "aether_media_fs.h"
#include "ui/app_scroll_list.h"
#include "app_text.h"
#include "src/misc/lv_text_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

enum { MUSIC_EQ_GAIN_ROW = AETHER_AUDIO_EQ_BANDS, MUSIC_EQ_ROWS = MUSIC_EQ_GAIN_ROW + 1 };
enum { LYRIC_PREVIOUS, LYRIC_ACTIVE, LYRIC_NEXT, LYRIC_PANEL_ROWS };
enum { MUSIC_SURFACE = 0x121D28, MUSIC_BUTTON = 0x253749, MUSIC_ACCENT = 0x79DBB7 };
enum { MUSIC_TRACK_CAPTION_WIDTH = 350 };

typedef struct {
    app_ui_t *ui;
    app_io_entry_t *entries;
    size_t count;
    app_scroll_list_t tracks;
    bool loading;
    bool dragging_seek;
    uint32_t drag_duration_ms;
    char drag_track[192];
    bool was_mounted;
    bool resume_checked;
    bool resume_directory;
    bool refresh_pending;
    unsigned catalog_retries;
    int64_t retry_at_us;
    char requested_path[192];
    char library_dir[192];
    lv_obj_t *player_view, *active_panel, *panel_launcher;
    void (*player_back)(app_ui_t *ui);
    lv_obj_t *title, *status, *elapsed, *duration, *track_count, *progress, *volume, *volume_label;
    char volume_caption[24];
    uint8_t shown_volume;
    lv_obj_t *play_label, *repeat_label, *library, *library_count;
    lv_obj_t *eq_panel, *eq_toggle_label, *eq_status;
    lv_obj_t *eq_sliders[MUSIC_EQ_ROWS], *eq_levels[MUSIC_EQ_ROWS];
    aether_audio_eq_t eq;
    lv_obj_t *lyric_rows[LYRIC_PANEL_ROWS];
    app_lyrics_t *lyrics;
    char lyric_song[192], lyric_request_song[192];
    uint32_t lyric_generation, lyric_request_generation;
    size_t lyric_active;
    unsigned lyric_attempt;
    bool lyric_loading;
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

static void music_cleanup(app_ui_t *ui);

static music_page_t *page(lv_event_t *e)
{
    app_ui_t *ui = lv_event_get_user_data(e);
    return ui && ui->page_cleanup == music_cleanup &&
        lv_obj_get_screen(lv_event_get_current_target_obj(e)) == ui->screen ? ui->page_context : NULL;
}

static void show_title(music_page_t *p, const char *path)
{
    char title[sizeof(p->requested_path)];
    strlcpy(title, filename(path), sizeof(title));
    char *dot = strrchr(title, '.');
    if (dot && dot != title) *dot = '\0';
    app_text_sanitize_utf8(title, strlen(title));
    app_ui_label_text(p->title, title);
}

static bool lyric_path(char out[192], const char *song, bool folder)
{
    const char *name = filename(song), *ext = strrchr(name, '.');
    if (!ext || ext == name || name == song) return false;
    if (!folder) {
        const size_t stem = (size_t)(ext - song);
        if (stem + sizeof(".lrc") > 192) return false;
        memcpy(out, song, stem);
        memcpy(out + stem, ".lrc", sizeof(".lrc"));
        return true;
    }
    const char *root = strchr(song + 1, '/');
    if (!root || (name - root == (ptrdiff_t)sizeof("/Lyrics/") - 1 &&
                  !strncasecmp(root, "/Lyrics/", sizeof("/Lyrics/") - 1))) return false;
    const int length = snprintf(out, 192, "%.*s/Lyrics/%.*s.lrc",
        (int)(root - song), song, (int)(ext - name), name);
    return length >= 0 && length < 192;
}

static void render_lyrics(music_page_t *p, uint32_t position)
{
    if (!p->lyrics) {
        app_ui_label_text(p->lyric_rows[LYRIC_PREVIOUS], "");
        app_ui_label_text(p->lyric_rows[LYRIC_NEXT], "");
        app_ui_label_text(p->lyric_rows[LYRIC_ACTIVE], !p->lyric_song[0] ? "请选择曲目" :
                          p->lyric_attempt < 2 || p->lyric_loading ? "正在匹配歌词…" : "暂无同步歌词");
        return;
    }
    const size_t index = app_lyrics_at(p->lyrics, position);
    if (index == p->lyric_active) return;
    p->lyric_active = index;
    app_ui_label_text(p->lyric_rows[LYRIC_PREVIOUS], app_lyrics_line(p->lyrics, index && index != SIZE_MAX ? index - 1 : SIZE_MAX));
    app_ui_label_text(p->lyric_rows[LYRIC_ACTIVE], app_lyrics_line(p->lyrics, index));
    app_ui_label_text(p->lyric_rows[LYRIC_NEXT], app_lyrics_line(p->lyrics, index == SIZE_MAX ? 0 : index + 1));
}

static void lyrics_loaded(app_ui_t *ui, const app_io_result_t *result)
{
    music_page_t *p = ui->page_cleanup == music_cleanup ? ui->page_context : NULL;
    if (!p || !p->lyric_loading || result->operation != APP_IO_READ_TEXT) return;
    p->lyric_loading = false;
    aether_audio_player_status_t player = {0};
    if (aether_audio_player_get_status(ui->audio_player, &player) != ESP_OK ||
        strcmp(player.path, p->lyric_request_song) || strcmp(player.path, p->lyric_song) ||
        !music_storage_ready(ui, player.path) ||
        aether_media_fs_generation(ui->storage, player.path) != p->lyric_request_generation) return;
    if (result->error == ESP_OK)
        p->lyrics = app_lyrics_parse(result->text, result->next_offset >= result->file_size);
    if (p->lyrics || (result->error != ESP_OK && result->error != ESP_ERR_NOT_FOUND)) p->lyric_attempt = 2;
    p->lyric_active = SIZE_MAX - 1;
    render_lyrics(p, player.position_ms);
}

static void update_lyrics(music_page_t *p, const aether_audio_player_status_t *player, bool mounted)
{
    const char *song = mounted ? player->path : "";
    const uint32_t generation = *song ? aether_media_fs_generation(p->ui->storage, song) : 0;
    if (strcmp(song, p->lyric_song) || generation != p->lyric_generation) {
        free(p->lyrics);
        p->lyrics = NULL;
        p->lyric_attempt = 0;
        p->lyric_generation = generation;
        strlcpy(p->lyric_song, song, sizeof(p->lyric_song));
    }
    if (*song && !p->loading && !p->refresh_pending && !p->lyric_loading && p->lyric_attempt < 2) {
        app_io_request_t request = {.operation = APP_IO_READ_TEXT, .limit = APP_LYRICS_READ_LIMIT};
        while (p->lyric_attempt < 2) {
            if (!lyric_path(request.path, song, p->lyric_attempt++ != 0)) continue;
            strlcpy(p->lyric_request_song, song, sizeof(p->lyric_request_song));
            p->lyric_request_generation = generation;
            p->lyric_loading = true;
            app_ui_request_io_ex(p->ui, &request, lyrics_loaded);
            break;
        }
    }
    render_lyrics(p, player->position_ms);
}

static lv_obj_t *button(music_page_t *p, lv_obj_t *parent, const char *text,
                        int x, int y, int width, int height, lv_event_cb_t callback)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, width, height);
    lv_obj_set_style_radius(b, 13, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(MUSIC_BUTTON), 0);
    lv_obj_add_event_cb(b, callback, LV_EVENT_CLICKED, p->ui);
    app_ui_add_text(b, text, LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, app_ui_chinese_font());
    return b;
}

static void music_panel_back(app_ui_t *ui)
{
    music_page_t *p = ui->page_context;
    if (!p || !p->active_panel) return;
    if (p->active_panel == p->library) {
        lv_obj_t *viewport = p->tracks.viewport;
        lv_obj_stop_scroll_anim(viewport);
        for (lv_indev_t *input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input)) {
            if (lv_indev_get_scroll_obj(input) == viewport) lv_indev_reset(input, viewport);
        }
    }
    const bool restore_focus = lv_obj_get_group(p->panel_launcher) != NULL;
    lv_obj_remove_flag(p->player_view, LV_OBJ_FLAG_HIDDEN);
    if (restore_focus) lv_group_focus_obj(p->panel_launcher);
    lv_obj_add_flag(p->active_panel, LV_OBJ_FLAG_HIDDEN);
    p->active_panel = p->panel_launcher = NULL;
    ui->page_back = p->player_back;
    p->player_back = NULL;
}

static void music_show_panel(music_page_t *p, lv_obj_t *panel, lv_obj_t *launcher)
{
    if (p->active_panel) return;
    p->player_back = p->ui->page_back;
    p->ui->page_back = music_panel_back;
    p->active_panel = panel;
    p->panel_launcher = launcher;
    p->dragging_seek = false;
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->player_view, LV_OBJ_FLAG_HIDDEN);
    lv_group_t *group = lv_obj_get_group(launcher);
    /* Focus the header before the panel rows so reopening a scrolled list
     * cannot jump to the first recycled button in the keypad group. */
    if (group && lv_group_get_focused(group) == launcher) lv_group_focus_prev(group);
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
    if (p->active_panel == p->library) music_panel_back(p->ui);
    show_title(p, path);
    app_ui_label_text(p->status, "正在切换歌曲…");
    return true;
}

static void select_track(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (!p || p->loading || p->active_panel != p->library) return;
    const size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (index < p->count) select_path(p, p->entries[index].path);
}

static void free_track_caption(lv_event_t *event)
{
    free(lv_event_get_user_data(event));
}

static lv_obj_t *create_track_row(lv_obj_t *parent, void *context)
{
    music_page_t *p = context;
    lv_obj_t *row = lv_button_create(parent);
    /* Use direct draw styles so fast drags don't recreate theme shadows or
     * start a size animation as the pointer passes through a row. */
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 382, 43);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(MUSIC_BUTTON), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x38566D), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x466A80), LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_add_event_cb(row, select_track, LV_EVENT_CLICKED, p->ui);
    const lv_font_t *font = app_ui_chinese_font();
    lv_obj_t *label = app_ui_add_text(row, "", LV_ALIGN_LEFT_MID, 12, 0, 0xFFFFFF, font);
    lv_obj_set_size(label, MUSIC_TRACK_CAPTION_WIDTH, lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    char *caption = calloc(1, sizeof(p->entries[0].path));
    if (caption && lv_obj_add_event_cb(label, free_track_caption, LV_EVENT_DELETE, caption)) {
        lv_obj_set_user_data(label, caption);
        lv_label_set_text_static(label, caption);
    } else {
        free(caption);
    }
    return row;
}

static void bind_track_row(lv_obj_t *row, size_t index, void *context)
{
    const music_page_t *p = context;
    lv_obj_t *label = lv_obj_get_child(row, 0);
    char name[sizeof(p->entries[index].path)];
    strlcpy(name, filename(p->entries[index].path), sizeof(name));
    const size_t length = app_text_sanitize_utf8(name, strlen(name));
    for (size_t i = 0; i < length; ++i) {
        if ((uint8_t)name[i] < 0x20) name[i] = ' ';
    }
    const lv_font_t *font = app_ui_chinese_font();
    lv_text_attributes_t attributes = {.max_width = MUSIC_TRACK_CAPTION_WIDTH,
                                       .text_flags = LV_TEXT_FLAG_BREAK_ALL};
    if (lv_text_get_next_line(name, length, font, NULL, &attributes) < length) {
        const char dots[] = "...";
        attributes.max_width -= lv_text_get_width(dots, sizeof(dots) - 1, font, &attributes);
        size_t end = lv_text_get_next_line(name, length, font, NULL, &attributes);
        if (end + sizeof(dots) > sizeof(name)) {
            end = sizeof(name) - sizeof(dots);
            while (((uint8_t)name[end] & 0xc0) == 0x80) --end;
        }
        while (end && name[end - 1] == ' ') --end;
        memcpy(name + end, dots, sizeof(dots));
    }
    char *caption = lv_obj_get_user_data(label);
    if (!caption) {
        app_ui_label_text(label, name);
    } else if (strcmp(caption, name) != 0) {
        strlcpy(caption, name, sizeof(name));
        lv_label_set_text_static(label, NULL);
    }
}

static void render_library(music_page_t *p)
{
    app_scroll_list_set_count(&p->tracks, p->count);
    if (p->count) app_ui_label_fmt(p->library_count, "%u 首", (unsigned)p->count);
    else app_ui_label_text(p->library_count, "曲库为空");
}

static void list_show(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (p) music_show_panel(p, p->library, lv_event_get_current_target_obj(e));
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
    music_show_panel(p, p->eq_panel, lv_event_get_current_target_obj(e));
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
    app_ui_add_text(p->eq_panel, "均衡器", LV_ALIGN_TOP_LEFT, 3, 9, 0xF6C175, app_ui_chinese_font());
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

static void refresh_catalog(music_page_t *p);

static bool use_resume_directory(music_page_t *p)
{
    char path[sizeof(p->library_dir)];
    if (aether_audio_player_get_resume_path(p->ui->audio_player, path, sizeof(path)) != ESP_OK ||
        !music_storage_ready(p->ui, path)) return false;
    char *name = strrchr(path, '/');
    if (!name || name == path) return false;
    *name = '\0';
    const char *root = aether_media_fs_default_root(p->ui->storage);
    const size_t root_length = strlen(root);
    /* The default catalog already scans its root and Music subdirectory. */
    if (!strcasecmp(path, root) ||
        (!strncasecmp(path, root, root_length) && !strcasecmp(path + root_length, "/Music"))) return false;
    strlcpy(p->library_dir, path, sizeof(p->library_dir));
    p->resume_directory = true;
    return true;
}

static void refresh_default_catalog(music_page_t *p)
{
    p->resume_directory = false;
    p->resume_checked = true;
    p->library_dir[0] = '\0';
    p->was_mounted = music_storage_ready(p->ui, NULL);
    p->catalog_retries = 0;
    refresh_catalog(p);
}

static bool restore_catalog_position(music_page_t *p, const app_io_result_t *result)
{
    if (p->resume_checked) return false;
    const bool prepared = result->error == ESP_OK && result->playlist_prepared;
    if (!prepared && result->error != ESP_ERR_NOT_FOUND) return false;

    const esp_err_t restored = prepared ? aether_audio_player_restore(p->ui->audio_player) : ESP_ERR_NOT_FOUND;
    if (restored == ESP_ERR_INVALID_STATE) {
        aether_audio_player_status_t status;
        if (aether_audio_player_get_status(p->ui->audio_player, &status) != ESP_OK ||
            (!status.active && !status.path[0])) {
            retry_catalog_later(p);
            return false;
        }
    }
    if (restored == ESP_ERR_NOT_FOUND) {
        if (p->resume_directory) {
            refresh_default_catalog(p);
            return true;
        }
        if (use_resume_directory(p)) {
            refresh_catalog(p);
            return true;
        }
    }
    if (prepared) p->resume_checked = true;
    return false;
}

static void loaded(app_ui_t *ui, const app_io_result_t *result)
{
    music_page_t *p = ui->page_context;
    if (!p) return;
    p->loading = false;
    if (result->playlist_prepared) {
        free(p->entries); p->entries = NULL; p->count = 0;
    }
    if (restore_catalog_position(p, result)) return;
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
    if (!p->retry_at_us) p->catalog_retries = 0;
    if (p->requested_path[0]) {
        if (select_path(p, p->requested_path)) p->requested_path[0] = '\0';
    } else {
        aether_audio_player_status_t status = {0};
        if (aether_audio_player_get_status(ui->audio_player, &status) == ESP_OK)
            show_title(p, status.path[0] ? status.path : p->entries[0].path);
    }
    if (result->truncated) app_ui_label_text(p->status, "曲库超过4096首，请分目录存放");
    render_library(p);
}

static void refresh_catalog(music_page_t *p)
{
    if (p->lyric_loading) { p->refresh_pending = true; return; }
    p->refresh_pending = false;
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
    if (aether_audio_player_get_status(p->ui->audio_player, &s) != ESP_OK || s.closing) return;
    if (s.active) {
        aether_audio_player_set_paused(p->ui->audio_player, s.state != AETHER_AUDIO_PAUSED);
    } else if (p->loading || (p->retry_at_us && !p->resume_checked)) return;
    else if (s.path[0]) {
        const esp_err_t error = aether_audio_player_play(p->ui->audio_player, s.path, s.volume);
        if (error != ESP_OK) app_ui_label_fmt(p->status, "播放失败：%s", esp_err_to_name(error));
    } else if (p->count) select_path(p, p->entries[0].path);
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
    if (aether_audio_player_get_status(p->ui->audio_player, &s) == ESP_OK)
        aether_audio_player_set_repeat(p->ui->audio_player, (s.repeat + 1) % 3);
}

static void show_volume(music_page_t *p, uint8_t volume)
{
    if (p->volume_caption[0] && p->shown_volume == volume) return;
    p->shown_volume = volume;
    snprintf(p->volume_caption, sizeof(p->volume_caption), "系统音量 %u%%", (unsigned)volume);
    lv_label_set_text_static(p->volume_label, p->volume_caption);
}

static void volume_event(lv_event_t *e)
{
    music_page_t *p = page(e);
    if (!p) return;
    const uint8_t value = (uint8_t)lv_slider_get_value(p->volume);
    const esp_err_t error = app_audio_system_set_volume(value, lv_event_get_code(e) == LV_EVENT_RELEASED);
    show_volume(p, value);
    if (error != ESP_OK) app_ui_label_fmt(p->status, "调整音量失败：%s", esp_err_to_name(error));
}

static void refresh_volume(music_page_t *p)
{
    app_audio_system_snapshot_t audio;
    app_audio_system_get_snapshot(&audio);
    if (!lv_obj_has_state(p->volume, LV_STATE_PRESSED))
        lv_slider_set_value(p->volume, audio.master_volume, LV_ANIM_OFF);
    show_volume(p, (uint8_t)lv_slider_get_value(p->volume));
    if (audio.ready) lv_obj_remove_state(p->volume, LV_STATE_DISABLED);
    else lv_obj_add_state(p->volume, LV_STATE_DISABLED);
}

static bool can_seek(const aether_audio_player_status_t *s)
{
    return s->active && !s->closing && s->duration_ms &&
        (s->state == AETHER_AUDIO_PLAYING || s->state == AETHER_AUDIO_PAUSED);
}

static bool same_seek_track(const music_page_t *p, const aether_audio_player_status_t *s)
{
    return can_seek(s) && p->drag_duration_ms == s->duration_ms && !strcmp(p->drag_track, s->path);
}

static uint32_t drag_position_ms(const music_page_t *p)
{
    return (uint32_t)((uint64_t)p->drag_duration_ms * lv_slider_get_value(p->progress) / 1000);
}

static void show_elapsed(music_page_t *p, uint32_t position_ms)
{
    const uint32_t seconds = position_ms / 1000;
    app_ui_label_fmt(p->elapsed, "%02lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
}

static void sync_progress(music_page_t *p, const aether_audio_player_status_t *s)
{
    if (p->dragging_seek && !same_seek_track(p, s)) p->dragging_seek = false;
    if (p->dragging_seek) return;
    const uint32_t position = can_seek(s) && s->seeking ? s->seek_requested_ms : s->position_ms;
    const uint32_t slider_ms = position > s->duration_ms ? s->duration_ms : position;
    lv_slider_set_value(p->progress, s->duration_ms ?
        (int32_t)(((uint64_t)slider_ms * 1000 + s->duration_ms / 2) / s->duration_ms) : 0, LV_ANIM_OFF);
    show_elapsed(p, position);
}

static void seek_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED &&
        code != LV_EVENT_PRESS_LOST && code != LV_EVENT_INDEV_RESET && code != LV_EVENT_VALUE_CHANGED) return;
    /* The old screen can still receive input and deletion events while its
     * successor owns the shared UI context. Only the live slider may seek. */
    music_page_t *p = page(e);
    if (!p || lv_event_get_current_target_obj(e) != p->progress || !p->ui->audio_player) return;
    if (code == LV_EVENT_VALUE_CHANGED) {
        if (p->dragging_seek) show_elapsed(p, drag_position_ms(p));
        return;
    }
    aether_audio_player_status_t s;
    if (aether_audio_player_get_status(p->ui->audio_player, &s) != ESP_OK || s.closing) {
        p->dragging_seek = false;
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        p->dragging_seek = can_seek(&s);
        if (p->dragging_seek) {
            p->drag_duration_ms = s.duration_ms;
            strlcpy(p->drag_track, s.path, sizeof(p->drag_track));
            show_elapsed(p, drag_position_ms(p));
        }
        return;
    }
    const bool commit = code == LV_EVENT_RELEASED && p->dragging_seek && same_seek_track(p, &s);
    p->dragging_seek = false;
    if (commit) {
        const uint32_t target = drag_position_ms(p);
        if (aether_audio_player_seek(p->ui->audio_player, target) == ESP_OK) {
            show_elapsed(p, target);
            return;
        }
    }
    sync_progress(p, &s);
}

static void music_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    music_page_t *p = ui->page_context;
    if (!p) return;
    if (!p->active_panel) refresh_volume(p);
    if (!ui->audio_player) return;
    aether_audio_player_status_t s = {0};
    /* The outgoing page can still be releasing a recording or song. */
    if (aether_audio_player_get_status(ui->audio_player, &s) != ESP_OK || s.closing) {
        p->dragging_seek = false;
        return;
    }
    const bool mounted = music_storage_ready(ui, s.active ? s.path : p->library_dir);
    if (!mounted) {
        p->dragging_seek = false;
        s.seeking = false;
        sync_progress(p, &s);
        update_lyrics(p, &s, false);
        if (p->resume_directory && !s.active && !p->loading && music_storage_ready(ui, NULL)) {
            refresh_default_catalog(p);
            return;
        }
        p->was_mounted = false;
        app_ui_label_text(p->status, "存储设备未就绪，请插卡或接入 USB 存储后刷新");
        return;
    }
    if (!p->was_mounted && !p->loading) {
        p->was_mounted = true;
        p->catalog_retries = 0;
        refresh_catalog(p);
    }
    if (p->refresh_pending && !p->loading && !p->lyric_loading) refresh_catalog(p);
    if (p->retry_at_us && !p->loading && esp_timer_get_time() >= p->retry_at_us) refresh_catalog(p);
    /* A subpage owns the visible surface. Resume transport updates and LRC
     * lookup on return, leaving the card bus and rendering time to playback
     * and the scrolling list in the meantime. */
    if (p->active_panel) return;
    if (s.path[0]) show_title(p, s.path);
    update_lyrics(p, &s, !s.path[0] || music_storage_ready(ui, s.path));
    sync_progress(p, &s);
    const uint32_t total = s.duration_ms / 1000;
    app_ui_label_fmt(p->duration, "%02lu:%02lu", (unsigned long)(total / 60), (unsigned long)(total % 60));
    if (s.track_count) app_ui_label_fmt(p->track_count, "%u / %u", s.track_index + 1, s.track_count);
    else app_ui_label_text(p->track_count, "");
    app_ui_label_text(p->play_label, s.state == AETHER_AUDIO_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    static const char *const modes[] = {"列表循环", "单曲循环", "随机播放"};
    app_ui_label_text(p->repeat_label, modes[s.repeat <= AETHER_AUDIO_SHUFFLE ? s.repeat : 0]);
    if (!p->loading) {
        if (s.state == AETHER_AUDIO_ERROR) app_ui_label_fmt(p->status, "播放失败：%s", esp_err_to_name(s.last_error));
        else if (s.state == AETHER_AUDIO_STARTING) app_ui_label_text(p->status, "正在打开音乐…");
        else if (s.seeking && can_seek(&s)) app_ui_label_text(p->status, "正在定位…");
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
    app_scroll_list_deinit(&p->tracks);
    lv_label_set_text_static(p->volume_label, "");
    free(p->entries);
    free(p->lyrics);
    free(p);
    ui->page_context = NULL;
    /* Leaving music stops the stream and releases decoder/codec/file/playlist
     * off LVGL. app_ui_new_screen already invalidated old preparation results. */
    (void)app_io_release_media(ui->io);
}

static void build_lyric_panel(music_page_t *p, lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, 0, 54);
    lv_obj_set_size(panel, 408, 115);
    lv_obj_set_style_radius(panel, 17, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1A2B37), 0);
    lv_obj_set_style_bg_grad_color(panel, lv_color_hex(0x15232B), 0);
    lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_VER, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    static const int offsets[] = {8, 30, 91};
    for (unsigned i = 0; i < LYRIC_PANEL_ROWS; ++i) {
        const bool active = i == LYRIC_ACTIVE;
        lv_obj_t *row = app_ui_add_text(panel, active ? "请选择曲目" : "", LV_ALIGN_TOP_MID, 0, offsets[i],
            active ? MUSIC_ACCENT : 0x91A8B8, app_ui_chinese_font_size(active ? 24 : 16));
        lv_obj_set_width(row, 378);
        lv_obj_set_height(row, active ? 60 : 20);
        lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
        p->lyric_rows[i] = row;
    }
}

static lv_obj_t *transport_button(music_page_t *p, lv_obj_t *parent, const char *icon,
                                   int x, bool primary, lv_event_cb_t callback)
{
    const int size = primary ? 58 : 48;
    lv_obj_t *control = button(p, parent, icon, x, primary ? 211 : 216, size, size, callback);
    lv_obj_set_style_radius(control, primary ? LV_RADIUS_CIRCLE : 20, 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(primary ? MUSIC_ACCENT : MUSIC_BUTTON), 0);
    lv_obj_set_ext_click_area(control, 4);
    lv_obj_t *symbol = lv_obj_get_child(control, 0);
    lv_obj_set_style_text_font(symbol, &lv_font_montserrat_24, 0);
    if (primary) lv_obj_set_style_text_color(symbol, lv_color_hex(0x152A23), 0);
    return symbol;
}

void app_open_music_path(app_ui_t *ui, const char *path)
{
    char requested[192] = {0};
    if (path) strlcpy(requested, path, sizeof(requested));
    lv_obj_t *body = app_ui_begin_page(ui, "音乐播放器");
    music_page_t *p = calloc(1, sizeof(*p));
    if (!p) { app_ui_add_text(body, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, app_ui_chinese_font()); return; }
    p->ui = ui;
    p->was_mounted = music_storage_ready(ui, requested);
    p->resume_checked = requested[0] != '\0';
    strlcpy(p->requested_path, requested, sizeof(p->requested_path));
    strlcpy(p->library_dir, requested, sizeof(p->library_dir));
    char *slash = strrchr(p->library_dir, '/');
    if (slash) *slash = '\0';
    else p->library_dir[0] = '\0';
    ui->page_context = p;
    ui->page_cleanup = music_cleanup;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(body, lv_color_hex(MUSIC_SURFACE), 0);
    lv_obj_t *content = p->player_view = lv_obj_create(body);
    lv_obj_remove_style_all(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(content, lv_pct(100), lv_pct(100));
    p->title = app_ui_add_text(content, "请选择音乐", LV_ALIGN_TOP_LEFT, 4, 0, 0xF0F7FF, app_ui_chinese_font_size(24));
    lv_obj_set_width(p->title, 396);
    lv_label_set_long_mode(p->title, LV_LABEL_LONG_DOT);
    p->status = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 4, 31, 0xA2B6C7, app_ui_chinese_font());
    lv_obj_set_width(p->status, 396);
    lv_label_set_long_mode(p->status, LV_LABEL_LONG_DOT);
    build_lyric_panel(p, content);
    p->progress = lv_slider_create(content);
    lv_obj_set_pos(p->progress, 9, 182); lv_obj_set_size(p->progress, 390, 8);
    lv_obj_set_ext_click_area(p->progress, 11);
    lv_obj_set_style_bg_color(p->progress, lv_color_hex(0x2B4352), LV_PART_MAIN);
    lv_obj_set_style_bg_color(p->progress, lv_color_hex(MUSIC_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(p->progress, lv_color_hex(MUSIC_ACCENT), LV_PART_KNOB);
    lv_slider_set_range(p->progress, 0, 1000);
    lv_obj_add_event_cb(p->progress, seek_event, LV_EVENT_ALL, ui);
    p->elapsed = app_ui_add_text(content, "00:00", LV_ALIGN_TOP_LEFT, 8, 192, 0xA8BECE, app_ui_chinese_font());
    p->duration = app_ui_add_text(content, "00:00", LV_ALIGN_TOP_RIGHT, -8, 192, 0xA8BECE, app_ui_chinese_font());
    p->track_count = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 192, 0x859CAB, app_ui_chinese_font());
    transport_button(p, content, LV_SYMBOL_PREV, 96, false, previous_event);
    p->play_label = transport_button(p, content, LV_SYMBOL_PLAY, 174, true, play_event);
    transport_button(p, content, LV_SYMBOL_NEXT, 264, false, next_event);
    transport_button(p, content, LV_SYMBOL_STOP, 350, false, stop_event);
    p->volume_label = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 285, 0xD9E8FF, app_ui_chinese_font());
    p->volume = lv_slider_create(content);
    lv_obj_set_pos(p->volume, 115, 284); lv_obj_set_size(p->volume, 278, 18);
    lv_obj_set_ext_click_area(p->volume, 14);
    lv_obj_set_style_transform_width(p->volume, 0, LV_PART_KNOB | LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(p->volume, 0, LV_PART_KNOB | LV_STATE_PRESSED);
    lv_slider_set_range(p->volume, 0, 100);
    lv_obj_add_event_cb(p->volume, volume_event, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(p->volume, volume_event, LV_EVENT_RELEASED, ui);
    refresh_volume(p);
    lv_obj_t *repeat = button(p, content, "列表循环", 0, 315, 112, 38, repeat_event);
    p->repeat_label = lv_obj_get_child(repeat, 0);
    button(p, content, "曲库", 121, 315, 81, 38, list_show);
    button(p, content, "刷新", 211, 315, 81, 38, refresh_event);
    lv_obj_t *eq = button(p, content, "均衡器", 301, 315, 103, 38, eq_show);
    if (!ui->audio_player) lv_obj_add_state(eq, LV_STATE_DISABLED);
    p->library = lv_obj_create(body);
    lv_obj_set_pos(p->library, 0, 0); lv_obj_set_size(p->library, 408, 354);
    lv_obj_set_style_pad_all(p->library, 8, 0);
    lv_obj_set_style_bg_color(p->library, lv_color_hex(MUSIC_SURFACE), 0);
    lv_obj_set_style_border_width(p->library, 0, 0);
    lv_obj_remove_flag(p->library, LV_OBJ_FLAG_SCROLLABLE);
    app_ui_add_text(p->library, "曲库", LV_ALIGN_TOP_LEFT, 3, 8, 0xF6C175, app_ui_chinese_font());
    p->library_count = app_ui_add_text(p->library, "曲库为空", LV_ALIGN_TOP_RIGHT, -4, 8, 0xF6C175, app_ui_chinese_font());
    lv_obj_t *rows = lv_obj_create(p->library);
    lv_obj_set_pos(rows, 0, 44);
    lv_obj_set_size(rows, 392, 294);
    lv_obj_set_style_pad_all(rows, 0, 0);
    lv_obj_set_style_border_width(rows, 0, 0);
    /* A solid viewport with the same fill as the library lets LVGL start a
     * list-only redraw here rather than repainting the surrounding layers. */
    lv_obj_set_style_radius(rows, 0, 0);
    lv_obj_set_style_bg_color(rows, lv_color_hex(MUSIC_SURFACE), 0);
    lv_obj_set_style_bg_opa(rows, LV_OPA_COVER, 0);
    app_scroll_list_init(&p->tracks, rows, 48, create_track_row, bind_track_row, p);
    lv_obj_add_flag(p->library, LV_OBJ_FLAG_HIDDEN);
    build_eq_panel(p, body);
    app_ui_set_timer(ui, music_timer, 120);
    if (!ui->audio_player) app_ui_label_text(p->status, "音频驱动未就绪，请检查 AC101S 供电和总线");
    else refresh_catalog(p);
}

void app_open_music(app_ui_t *ui)
{
    app_open_music_path(ui, NULL);
}
