#include "app_ui_internal.h"
#include "app_reader_layout.h"
#include "aether_media_fs.h"
#include "ui/app_scroll_list.h"
#include "app_text.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define READER_WIDTH 448U
#define READER_HEIGHT 448U
#define READER_MARGIN 16
#define READER_LINE_SPACE 4U
#define READER_CHUNK 16384U
#define READER_HISTORY 512U
#define READER_TOOLBAR_MS 3000U
#define READER_TAP_DISTANCE 18
#define READER_SWIPE_DISTANCE 48
#define SHELF_WIDTH 424

typedef enum {
    READ_RESUME, READ_FORWARD, READ_PREVIOUS, READ_REFLOW, READ_SEEK, READ_BACK_SCAN
} reader_request_kind_t;

typedef struct {
    app_ui_t *ui;
    app_io_entry_t *books;
    size_t book_count;
    app_scroll_list_t shelf;
    bool truncated;
    bool reading;
    bool loading;
    bool dark;
    bool dirty;
    bool card_was_mounted;
    bool reload_after_insert;
    bool bookmark_pending;
    esp_err_t last_save_error;
    uint32_t font_px;
    int64_t save_at_us;
    uint64_t offset;
    uint64_t next_offset;
    uint64_t file_size;
    uint64_t text_start_offset;
    uint64_t before_offset;
    app_reader_encoding_t encoding;
    uint32_t invalid_count, missing_count;
    unsigned encoding_hint;
    app_reader_turn_mode_t turn_mode;
    lv_indev_t *press_device;
    lv_point_t press_start;
    bool press_moved;
    bool press_long;
    uint32_t toolbar_shown;
    uint64_t history[READER_HISTORY];
    size_t history_count;
    reader_request_kind_t request_kind;
    char path[192];
    char page[4096];
    lv_obj_t *body;
    lv_obj_t *position;
    lv_obj_t *seek;
    lv_obj_t *toolbar;
    lv_obj_t *notice;
    lv_obj_t *font_label;
    lv_obj_t *theme_label;
    lv_obj_t *encoding_label;
    lv_obj_t *turn_label;
} reader_t;

static void show_shelf(reader_t *reader);
static void request_page(reader_t *reader, uint64_t offset, reader_request_kind_t kind);
static void previous_page(reader_t *reader);
static void next_page(reader_t *reader);
static void reader_timer(lv_timer_t *timer);

static reader_t *get_reader(app_ui_t *ui)
{
    return ui ? ui->page_context : NULL;
}

static bool reader_storage_ready(const reader_t *reader)
{
    app_ui_t *ui = reader->ui;
    if (app_io_usb_storage_blocked(ui->io)) return false;
    if (!reader->reading) {
        const char *root = aether_media_fs_default_root(ui->storage);
        return aether_media_fs_path_ready(ui->storage, root);
    }
    return !strncmp(reader->path, "/usb/", 5) ?
        aether_media_fs_path_ready(ui->storage, reader->path) : aether_storage_is_mounted(ui->storage);
}

static const char *file_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static lv_obj_t *reader_button(reader_t *reader, lv_obj_t *parent,
                                const char *caption, int x, int y, int width,
                                lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 38);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x215C50), 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, reader->ui);
    app_ui_add_text(button, caption, LV_ALIGN_CENTER, 0, 0, 0xFFFFFF,
                    app_ui_chinese_font_size(16));
    return button;
}

static void mark_dirty(reader_t *reader)
{
    reader->dirty = true;
    reader->save_at_us = esp_timer_get_time() + 2000000;
}

static void save_bookmark(reader_t *reader)
{
    if (!reader || !reader->dirty || !reader->path[0]) return;
    app_io_request_t request = {
        .operation = APP_IO_READER_SAVE,
        .generation = reader->ui->page_generation,
        .offset = reader->offset,
        .font_px = reader->font_px,
        .dark = reader->dark,
        .encoding_hint = reader->encoding_hint,
        .turn_mode = reader->turn_mode,
    };
    strlcpy(request.path, reader->path, sizeof(request.path));
    if (app_io_submit_request(reader->ui->io, &request) == ESP_OK) reader->dirty = false;
}

static void reader_cleanup(app_ui_t *ui)
{
    reader_t *reader = get_reader(ui);
    if (!reader) return;
    save_bookmark(reader);
    app_scroll_list_deinit(&reader->shelf);
    free(reader->books);
    free(reader);
    ui->page_context = NULL;
}

static uint16_t glyph_width(uint32_t codepoint, uint32_t next, void *context)
{
    return lv_font_get_glyph_width(context, codepoint, next);
}

static app_reader_page_t layout(reader_t *reader, const char *text, size_t length)
{
    const lv_font_t *font = app_ui_chinese_font_size(reader->font_px);
    return app_reader_layout_page(text, length, reader->page, sizeof(reader->page),
                                   READER_WIDTH, READER_HEIGHT,
                                   (unsigned)lv_font_get_line_height(font), READER_LINE_SPACE,
                                   glyph_width, (void *)font);
}

static void apply_theme(reader_t *reader)
{
    const uint32_t background = reader->dark ? 0x10192A : 0xF3E9D3;
    const uint32_t foreground = reader->dark ? 0xD8E2EC : 0x302D27;
    lv_obj_set_style_bg_color(reader->ui->content, lv_color_hex(background), 0);
    lv_obj_set_style_text_color(reader->body, lv_color_hex(foreground), 0);
    lv_obj_set_style_text_font(reader->body, app_ui_chinese_font_size(reader->font_px), 0);
    app_ui_label_fmt(reader->font_label, "%u号字", (unsigned)reader->font_px);
    app_ui_label_text(reader->theme_label, reader->dark ? "日间" : "夜间");
}

static void reader_notice(reader_t *reader, const char *text)
{
    app_ui_label_text(reader->notice, text);
    if (*text) lv_obj_remove_flag(reader->notice, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(reader->notice, LV_OBJ_FLAG_HIDDEN);
}

static void update_position(reader_t *reader)
{
    const uint64_t progress = reader->file_size ?
        (reader->next_offset >= reader->file_size ? 1000U :
         reader->offset * 1000U / reader->file_size) : 0;
    app_ui_label_fmt(reader->position, "%" PRIu64 ".%" PRIu64 "%% · %s · 缺字%u/坏码%u%s",
                      progress / 10U, progress % 10U,
                      app_reader_encoding_name(reader->encoding), (unsigned)reader->missing_count, (unsigned)reader->invalid_count,
                      app_io_bookmark_pending(reader->ui->io) ? "  待保存" :
                      reader->next_offset >= reader->file_size && reader->file_size ? "  完" : "");
    lv_slider_set_value(reader->seek, (int32_t)progress, LV_ANIM_OFF);
}

static uint64_t source_position(const app_io_result_t *result, size_t utf8_byte)
{
    return result->source_offsets ? result->source_offsets[utf8_byte] : result->offset + utf8_byte;
}

static void render_page(reader_t *reader, const app_io_result_t *result, size_t relative,
                         size_t length)
{
    const char *text = result->text ? result->text : "";
    if (relative > length) relative = length;
    const app_reader_page_t page = layout(reader, text + relative, length - relative);
    if (reader->request_kind == READ_FORWARD && reader->offset != source_position(result, relative)) {
        if (reader->history_count == READER_HISTORY) {
            memmove(reader->history, reader->history + 1,
                      (READER_HISTORY - 1U) * sizeof(reader->history[0]));
            --reader->history_count;
        }
        reader->history[reader->history_count++] = reader->offset;
    } else if (reader->request_kind == READ_PREVIOUS && reader->history_count) {
        --reader->history_count;
    }
    reader->offset = source_position(result, relative);
    reader->next_offset = source_position(result, relative + page.consumed);
    if (relative + page.consumed == length) {
        const uint64_t boundary = reader->request_kind == READ_BACK_SCAN ?
            reader->before_offset : result->next_offset;
        if (boundary > reader->next_offset) reader->next_offset = boundary;
    }
    reader->file_size = result->file_size;
    reader->encoding = result->encoding; reader->invalid_count = result->invalid_count; reader->missing_count = 0;
    const lv_font_t *font = app_ui_chinese_font_size(reader->font_px);
    const size_t page_length = strlen(reader->page);
    for (size_t cursor = 0; cursor < page_length;) {
        size_t bytes;
        uint32_t cp = app_reader_codepoint(reader->page + cursor, page_length - cursor, &bytes);
        cursor += bytes;
        lv_font_glyph_dsc_t glyph;
        if (cp > 0x20 && (!lv_font_get_glyph_dsc(font, &glyph, cp, 0) || glyph.is_placeholder)) ++reader->missing_count;
    }
    reader->text_start_offset = result->text_start_offset;
    if (reader->next_offset > reader->file_size) reader->next_offset = reader->file_size;
    reader->loading = false;
    reader->reload_after_insert = false;
    app_ui_label_text(reader->body, page.written ? reader->page :
                       reader->file_size ? "已到书末" : "这本书是空文件");
    reader_notice(reader, "");
    update_position(reader);
    mark_dirty(reader);
}

static void page_loaded(app_ui_t *ui, const app_io_result_t *result)
{
    reader_t *reader = get_reader(ui);
    if (!reader || !reader->reading) return;
    reader->loading = false;
    if (result->error != ESP_OK) {
        char message[96];
        snprintf(message, sizeof(message), "读取失败：%s", esp_err_to_name(result->error));
        reader_notice(reader, message);
        return;
    }
    if (reader->request_kind == READ_RESUME && result->file_size &&
        reader->offset > result->file_size) {
        reader->history_count = 0;
        request_page(reader, 0, READ_SEEK);
        return;
    }
    const size_t length = result->text ? strlen(result->text) : 0;
    if (reader->request_kind == READ_BACK_SCAN && result->text) {
        if (result->next_offset < reader->before_offset) {
            reader_notice(reader, "回扫数据不足，请重新选择阅读位置");
            return;
        }
        /* Reverse pagination after reopening has no history. Stop the last
         * reconstructed page exactly at the current page's source boundary. */
        size_t end = length;
        size_t lower = 0;
        while (lower < end) {
            const size_t mid = lower + (end - lower) / 2U;
            if (source_position(result, mid) < reader->before_offset) lower = mid + 1U;
            else end = mid;
        }
        size_t relative = 0;
        while (relative < end) {
            const app_reader_page_t page = layout(reader, result->text + relative, end - relative);
            if (!page.consumed || page.consumed == end - relative) break;
            relative += page.consumed;
        }
        render_page(reader, result, relative, end);
    } else {
        render_page(reader, result, 0, length);
    }
}

static void request_page(reader_t *reader, uint64_t offset, reader_request_kind_t kind)
{
    if (!reader || reader->loading) return;
    if (!reader_storage_ready(reader)) {
        reader_notice(reader, "存储设备未就绪");
        return;
    }
    reader->loading = true;
    reader->request_kind = kind;
    app_io_request_t request = {.operation = APP_IO_READ_TEXT,
                                 .offset = offset, .limit = kind == READ_BACK_SCAN ? 2U * READER_CHUNK : READER_CHUNK};
    request.source_boundary = kind == READ_FORWARD || kind == READ_PREVIOUS || kind == READ_REFLOW;
    request.encoding_hint = reader->encoding_hint;
    strlcpy(request.path, reader->path, sizeof(request.path));
    app_ui_label_text(reader->position, "正在排版…");
    app_ui_request_io_ex(reader->ui, &request, page_loaded);
}

static void next_page(reader_t *reader)
{
    if (reader && !reader->loading && reader->next_offset < reader->file_size)
        request_page(reader, reader->next_offset, READ_FORWARD);
}

static void previous_page(reader_t *reader)
{
    if (!reader || reader->loading || reader->offset <= reader->text_start_offset) return;
    if (reader->history_count) {
        request_page(reader, reader->history[reader->history_count - 1U], READ_PREVIOUS);
    } else {
        reader->before_offset = reader->offset;
        request_page(reader, reader->offset > READER_CHUNK ? reader->offset - READER_CHUNK : 0,
                       READ_BACK_SCAN);
    }
}

static void show_toolbar(reader_t *reader)
{
    reader->toolbar_shown = lv_tick_get();
    lv_obj_remove_flag(reader->toolbar, LV_OBJ_FLAG_HIDDEN);
}

static void font_event(lv_event_t *event)
{
    reader_t *reader = get_reader(lv_event_get_user_data(event));
    if (!reader || reader->loading) return;
    show_toolbar(reader);
    if (!reader_storage_ready(reader)) {
        reader_notice(reader, "存储设备未就绪");
        return;
    }
    reader->font_px = reader->font_px == 24 ? 16 : 24;
    reader->history_count = 0;
    apply_theme(reader);
    mark_dirty(reader);
    request_page(reader, reader->offset, READ_REFLOW);
}

static void theme_event(lv_event_t *event)
{
    reader_t *reader = get_reader(lv_event_get_user_data(event));
    if (!reader) return;
    show_toolbar(reader);
    reader->dark = !reader->dark;
    apply_theme(reader);
    mark_dirty(reader);
}

static void encoding_event(lv_event_t *event)
{
    reader_t *reader = get_reader(lv_event_get_user_data(event));
    if (!reader || reader->loading) return;
    show_toolbar(reader);
    reader->encoding_hint = (reader->encoding_hint + 1U) % 5U;
    static const char *const captions[] = {"自动编码", "UTF-8", "GBK", "UTF-16LE", "UTF-16BE"};
    app_ui_label_text(reader->encoding_label, captions[reader->encoding_hint]);
    reader->history_count = 0;
    mark_dirty(reader);
    request_page(reader, reader->offset, READ_SEEK);
}

static void bookshelf_event(lv_event_t *event)
{
    app_open_reader(lv_event_get_user_data(event));
}

static void seek_event(lv_event_t *event)
{
    reader_t *reader = get_reader(lv_event_get_user_data(event));
    if (!reader || reader->loading || !reader->file_size) return;
    show_toolbar(reader);
    const int32_t value = lv_slider_get_value(reader->seek);
    reader->history_count = 0;
    if (value >= 1000) {
        reader->before_offset = reader->file_size;
        request_page(reader, reader->file_size > READER_CHUNK ? reader->file_size - READER_CHUNK : 0,
                       READ_BACK_SCAN);
    } else {
        request_page(reader, reader->file_size * (uint32_t)value / 1000U, READ_SEEK);
    }
}

static const char *const turn_mode_names[] = {"左右滑动", "左右点击", "上下滑动", "上下点击"};

static void turn_mode_event(lv_event_t *event)
{
    reader_t *reader = get_reader(lv_event_get_user_data(event));
    if (!reader || reader->loading) return;
    reader->turn_mode = (reader->turn_mode + 1U) % APP_READER_TURN_MODE_COUNT;
    app_ui_label_text(reader->turn_label, turn_mode_names[reader->turn_mode]);
    show_toolbar(reader);
    mark_dirty(reader);
}

static void reader_press_event(lv_event_t *event)
{
    reader_t *reader = get_reader(lv_event_get_user_data(event));
    if (!reader || !reader->reading) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET) {
        if (lv_event_get_param(event) == reader->press_device) reader->press_device = NULL;
        return;
    }
    lv_indev_t *device = lv_event_get_indev(event);
    if (!device || lv_indev_get_type(device) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t point;
    lv_indev_get_point(device, &point);
    if (code == LV_EVENT_PRESSED) {
        reader->press_device = device;
        reader->press_start = point;
        reader->press_moved = false;
        reader->press_long = false;
        return;
    }
    if (reader->press_device != device) return;
    const int dx = point.x - reader->press_start.x;
    const int dy = point.y - reader->press_start.y;
    if (abs(dx) > READER_TAP_DISTANCE || abs(dy) > READER_TAP_DISTANCE) reader->press_moved = true;
    if (code == LV_EVENT_LONG_PRESSED && !reader->press_moved) {
        reader->press_long = true;
        show_toolbar(reader);
    }
    if (code != LV_EVENT_RELEASED) return;
    reader->press_device = NULL;
    if (reader->press_long || reader->loading) return;

    const bool vertical = reader->turn_mode == APP_READER_TURN_SWIPE_VERTICAL ||
                          reader->turn_mode == APP_READER_TURN_TAP_VERTICAL;
    const bool tap = reader->turn_mode == APP_READER_TURN_TAP_HORIZONTAL ||
                     reader->turn_mode == APP_READER_TURN_TAP_VERTICAL;
    bool forward;
    if (tap) {
        if (reader->press_moved) return;
        lv_area_t bounds;
        lv_obj_get_coords(reader->ui->content, &bounds);
        forward = vertical ? point.y >= (bounds.y1 + bounds.y2 + 1) / 2 :
                             point.x >= (bounds.x1 + bounds.x2 + 1) / 2;
    } else {
        const int along = vertical ? dy : dx;
        const int across = vertical ? dx : dy;
        if (abs(along) < READER_SWIPE_DISTANCE || abs(along) <= abs(across)) return;
        forward = along < 0;
    }
    if (forward) next_page(reader);
    else previous_page(reader);
}

static void build_reader(reader_t *reader)
{
    lv_obj_t *content = reader->ui->content;
    app_scroll_list_deinit(&reader->shelf);
    lv_obj_clean(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(content, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_center(content);
    lv_obj_move_foreground(content);
    const lv_event_code_t input_events[] = {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED,
        LV_EVENT_LONG_PRESSED, LV_EVENT_PRESS_LOST, LV_EVENT_INDEV_RESET};
    for (size_t i = 0; i < sizeof(input_events) / sizeof(input_events[0]); ++i)
        lv_obj_add_event_cb(content, reader_press_event, input_events[i], reader->ui);
    reader->body = app_ui_add_text(content, "正在恢复阅读位置…",
                                     LV_ALIGN_TOP_LEFT, READER_MARGIN, READER_MARGIN, 0xD8E2EC,
                                     app_ui_chinese_font_size(reader->font_px));
    lv_obj_set_size(reader->body, READER_WIDTH, READER_HEIGHT);
    lv_label_set_long_mode(reader->body, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_line_space(reader->body, READER_LINE_SPACE, 0);
    lv_obj_set_style_text_letter_space(reader->body, 0, 0);
    reader->notice = app_ui_add_text(content, "", LV_ALIGN_BOTTOM_MID, 0, -8, 0xFFDFB7,
                                        app_ui_chinese_font_size(16));
    lv_obj_set_width(reader->notice, READER_WIDTH);
    lv_obj_set_style_pad_all(reader->notice, 6, 0);
    lv_obj_set_style_bg_color(reader->notice, lv_color_hex(0x342521), 0);
    lv_obj_set_style_bg_opa(reader->notice, LV_OPA_COVER, 0);
    lv_obj_add_flag(reader->notice, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *toolbar = lv_obj_create(content);
    reader->toolbar = toolbar;
    lv_obj_set_size(toolbar, 472, 142);
    lv_obj_align(toolbar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_pad_all(toolbar, 12, 0);
    lv_obj_set_style_border_width(toolbar, 0, 0);
    lv_obj_set_style_radius(toolbar, 18, 0);
    lv_obj_set_style_bg_color(toolbar, lv_color_hex(0x14282D), 0);
    lv_obj_set_style_bg_opa(toolbar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *title = app_ui_add_text(toolbar, file_name(reader->path),
                                      LV_ALIGN_TOP_LEFT, 0, 0, 0xE6EDF8,
                                      app_ui_chinese_font_size(16));
    lv_obj_set_width(title, READER_WIDTH);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    reader->position = app_ui_add_text(toolbar, "", LV_ALIGN_TOP_LEFT, 0, 23,
                                        0xABC6CC, app_ui_chinese_font_size(16));
    lv_obj_set_width(reader->position, READER_WIDTH);
    reader->seek = lv_slider_create(toolbar);
    lv_obj_set_pos(reader->seek, 8, 58);
    lv_obj_set_size(reader->seek, READER_WIDTH - 16U, 8);
    lv_obj_set_ext_click_area(reader->seek, 10);
    lv_slider_set_range(reader->seek, 0, 1000);
    lv_obj_add_event_cb(reader->seek, seek_event, LV_EVENT_RELEASED, reader->ui);
    lv_obj_t *button = reader_button(reader, toolbar, "24号字", 0, 80, 72, font_event);
    reader->font_label = lv_obj_get_child(button, 0);
    button = reader_button(reader, toolbar, "日间", 80, 80, 68, theme_event);
    reader->theme_label = lv_obj_get_child(button, 0);
    button = reader_button(reader, toolbar, "自动编码", 156, 80, 98, encoding_event);
    reader->encoding_label = lv_obj_get_child(button, 0);
    button = reader_button(reader, toolbar, "左右滑动", 262, 80, 102, turn_mode_event);
    reader->turn_label = lv_obj_get_child(button, 0);
    reader_button(reader, toolbar, "书架", 372, 80, 76, bookshelf_event);
    lv_obj_add_flag(toolbar, LV_OBJ_FLAG_HIDDEN);
    apply_theme(reader);
}

static void bookmark_loaded(app_ui_t *ui, const app_io_result_t *result)
{
    reader_t *reader = get_reader(ui);
    if (!reader || !reader->reading) return;
    reader->loading = false;
    if (result->error == ESP_OK) {
        reader->offset = result->offset;
        reader->font_px = result->font_px == 16 ? 16 : 24;
        reader->dark = result->dark;
        reader->encoding_hint = result->encoding_hint <= 4 ? result->encoding_hint : 0;
        if ((unsigned)result->turn_mode < APP_READER_TURN_MODE_COUNT) reader->turn_mode = result->turn_mode;
    }
    apply_theme(reader);
    static const char *const captions[] = {"自动编码", "UTF-8", "GBK", "UTF-16LE", "UTF-16BE"};
    app_ui_label_text(reader->encoding_label, captions[reader->encoding_hint]);
    app_ui_label_text(reader->turn_label, turn_mode_names[reader->turn_mode]);
    request_page(reader, reader->offset, READ_RESUME);
}

static void book_event(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    reader_t *reader = get_reader(ui);
    const size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (!reader || reader->loading || index >= reader->book_count) return;
    reader->reading = true;
    reader->offset = reader->next_offset = 0;
    reader->file_size = reader->books[index].size;
    strlcpy(reader->path, reader->books[index].path, sizeof(reader->path));
    reader->card_was_mounted = reader_storage_ready(reader);
    build_reader(reader);
    free(reader->books);
    reader->books = NULL;
    reader->book_count = 0;
    lv_timer_set_period(ui->timer, 100);
    app_io_request_t request = {.operation = APP_IO_READER_LOAD};
    strlcpy(request.path, reader->path, sizeof(request.path));
    reader->loading = true;
    app_ui_request_io_ex(ui, &request, bookmark_loaded);
}

void app_open_reader_path(app_ui_t *ui, const char *path)
{
    /* The caller may pass a path held by the page being destroyed. */
    char selected[192];
    if (!path || !path[0] || strlen(path) >= sizeof(selected)) return;
    strlcpy(selected, path, sizeof(selected));
    lv_obj_t *content = app_ui_begin_page(ui, "小说阅读");
    reader_t *reader = heap_caps_calloc(1, sizeof(*reader), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!reader) {
        app_ui_add_text(content, "阅读器内存分配失败", LV_ALIGN_TOP_LEFT, 0, 0,
                          0xFF8A8A, app_ui_chinese_font_size(16));
        return;
    }
    reader->ui = ui;
    reader->font_px = 24;
    reader->dark = true;
    reader->reading = true;
    strlcpy(reader->path, selected, sizeof(reader->path));
    reader->card_was_mounted = reader_storage_ready(reader);
    ui->page_context = reader;
    ui->page_cleanup = reader_cleanup;
    build_reader(reader);
    app_ui_set_timer(ui, reader_timer, 100);
    app_io_request_t request = {.operation = APP_IO_READER_LOAD};
    strlcpy(request.path, selected, sizeof(request.path));
    reader->loading = true;
    app_ui_request_io_ex(ui, &request, bookmark_loaded);
}

static lv_obj_t *create_shelf_row(lv_obj_t *parent, void *context)
{
    reader_t *reader = context;
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, SHELF_WIDTH - 8, 44);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x172E39), 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_add_event_cb(row, book_event, LV_EVENT_CLICKED, reader->ui);
    lv_obj_t *name = app_ui_add_text(row, "", LV_ALIGN_LEFT_MID, 0, 0, 0xE6EDF8,
                                     app_ui_chinese_font_size(16));
    lv_obj_set_width(name, 310);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    app_ui_add_text(row, "", LV_ALIGN_RIGHT_MID, 0, 0, 0x8FA9CD, app_ui_chinese_font_size(16));
    return row;
}

static void bind_shelf_row(lv_obj_t *row, size_t index, void *context)
{
    const reader_t *reader = context;
    const app_io_entry_t *book = &reader->books[index];
    char name[sizeof(book->path)];
    strlcpy(name, file_name(book->path), sizeof(name));
    app_text_sanitize_utf8(name, strlen(name));
    app_ui_label_text(lv_obj_get_child(row, 0), name);
    app_ui_label_fmt(lv_obj_get_child(row, 1), "%" PRIu64 " KB", (book->size + 1023U) / 1024U);
}

static void show_shelf(reader_t *reader)
{
    lv_obj_t *content = reader->ui->content;
    app_scroll_list_deinit(&reader->shelf);
    lv_obj_clean(content);
    lv_obj_set_style_pad_all(content, 12, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    reader->position = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 9,
                                        0xABC6CC, app_ui_chinese_font_size(16));
    if (reader->book_count)
        app_ui_label_fmt(reader->position, "%u 本%s", (unsigned)reader->book_count,
                          reader->truncated ? " · 索引已达上限" : "");
    else app_ui_label_text(reader->position, "Books 文件夹和根目录没有 TXT");
    reader_button(reader, content, "刷新", 344, 0, 72, bookshelf_event);
    lv_obj_t *viewport = lv_obj_create(content);
    lv_obj_set_pos(viewport, 0, 48);
    lv_obj_set_size(viewport, SHELF_WIDTH, 322);
    lv_obj_set_style_pad_all(viewport, 0, 0);
    lv_obj_set_style_border_width(viewport, 0, 0);
    lv_obj_set_style_bg_opa(viewport, LV_OPA_TRANSP, 0);
    app_scroll_list_init(&reader->shelf, viewport, 50, create_shelf_row, bind_shelf_row, reader);
    app_scroll_list_set_count(&reader->shelf, reader->book_count);
}

static void books_loaded(app_ui_t *ui, const app_io_result_t *result)
{
    reader_t *reader = get_reader(ui);
    if (!reader || reader->reading) return;
    reader->loading = false;
    free(reader->books);
    reader->books = NULL;
    reader->book_count = 0;
    if (result->error == ESP_OK && result->count) {
        reader->books = heap_caps_malloc(result->count * sizeof(*reader->books),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (reader->books) {
            memcpy(reader->books, result->entries, result->count * sizeof(*reader->books));
            reader->book_count = result->count;
        }
    }
    reader->truncated = result->truncated;
    show_shelf(reader);
    if (result->error != ESP_OK)
        app_ui_label_fmt(reader->position, "书架不可用：%s", esp_err_to_name(result->error));
    else if (result->count && !reader->books)
        app_ui_label_text(reader->position, "内存不足，请退出其他应用后重试");
}

static void request_books(reader_t *reader)
{
    reader->loading = true;
    const app_io_request_t request = {.operation = APP_IO_CATALOG, .filter = APP_IO_BOOK,
                                       .limit = 4096};
    app_ui_request_io_ex(reader->ui, &request, books_loaded);
}

static void reader_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    reader_t *reader = get_reader(ui);
    if (!reader) return;
    if (reader->reading && !lv_obj_has_flag(reader->toolbar, LV_OBJ_FLAG_HIDDEN) &&
        lv_tick_elaps(reader->toolbar_shown) >= READER_TOOLBAR_MS &&
        !lv_obj_has_state(reader->seek, LV_STATE_PRESSED))
        lv_obj_add_flag(reader->toolbar, LV_OBJ_FLAG_HIDDEN);
    if (reader->dirty && esp_timer_get_time() >= reader->save_at_us) save_bookmark(reader);
    const esp_err_t save_error = app_io_bookmark_status(ui->io);
    const bool pending = app_io_bookmark_pending(ui->io);
    if (reader->reading && (save_error != reader->last_save_error || pending != reader->bookmark_pending)) {
        reader->last_save_error = save_error;
        reader->bookmark_pending = pending;
        if (save_error != ESP_OK) app_ui_label_text(reader->position, "书签保存失败，后台重试中");
        else if (!reader->loading) update_position(reader);
    }
    const bool mounted = reader_storage_ready(reader);
    if (reader->card_was_mounted && !mounted) {
        reader->reload_after_insert = true;
        save_bookmark(reader);
        if (reader->reading) reader_notice(reader, "存储设备已移除，阅读位置已保留");
        else if (reader->position) app_ui_label_text(reader->position, "存储设备已移除");
    }
    if (mounted && !reader->loading &&
        (!reader->card_was_mounted || reader->reload_after_insert)) {
        reader->reload_after_insert = false;
        if (reader->reading) request_page(reader, reader->offset, READ_RESUME);
        else request_books(reader);
    }
    reader->card_was_mounted = mounted;
}

void app_open_reader(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "小说阅读");
    reader_t *reader = heap_caps_calloc(1, sizeof(*reader), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!reader) {
        app_ui_add_text(content, "阅读器内存分配失败", LV_ALIGN_TOP_LEFT, 0, 0,
                          0xFF8A8A, app_ui_chinese_font_size(16));
        return;
    }
    reader->ui = ui;
    reader->font_px = 24;
    reader->dark = true;
    reader->card_was_mounted = reader_storage_ready(reader);
    ui->page_context = reader;
    ui->page_cleanup = reader_cleanup;
    app_ui_add_text(content, "正在扫描书架…", LV_ALIGN_TOP_LEFT, 0, 0,
                      0xC9DCF8, app_ui_chinese_font_size(16));
    app_ui_set_timer(ui, reader_timer, 500);
    request_books(reader);
}
