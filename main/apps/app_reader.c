#include "app_ui_internal.h"
#include "app_reader_layout.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define READER_WIDTH 424U
#define READER_HEIGHT 260U
#define READER_LINE_SPACE 4U
#define READER_CHUNK 16384U
#define READER_HISTORY 512U
#define SHELF_PAGE_SIZE 6U

typedef enum {
    READ_RESUME, READ_FORWARD, READ_PREVIOUS, READ_REFLOW, READ_SEEK, READ_BACK_SCAN
} reader_request_kind_t;

typedef struct {
    app_ui_t *ui;
    app_io_entry_t *books;
    size_t book_count;
    size_t shelf_page;
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
    uint64_t history[READER_HISTORY];
    size_t history_count;
    reader_request_kind_t request_kind;
    char path[192];
    char page[4096];
    lv_obj_t *title;
    lv_obj_t *body;
    lv_obj_t *position;
    lv_obj_t *seek;
    lv_obj_t *previous;
    lv_obj_t *next;
    lv_obj_t *font_label;
    lv_obj_t *theme_label;
    lv_obj_t *encoding_label;
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

static bool reader_storage_ready(app_ui_t *ui)
{
    return aether_storage_is_mounted(ui->storage) && !app_io_usb_storage_blocked(ui->io);
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
    };
    strlcpy(request.path, reader->path, sizeof(request.path));
    if (app_io_submit_request(reader->ui->io, &request) == ESP_OK) reader->dirty = false;
}

static void reader_cleanup(app_ui_t *ui)
{
    reader_t *reader = get_reader(ui);
    if (!reader) return;
    save_bookmark(reader);
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
    lv_obj_set_style_text_color(reader->title, lv_color_hex(foreground), 0);
    lv_obj_set_style_text_color(reader->position,
                                 lv_color_hex(reader->dark ? 0x91ABBD : 0x655E4D), 0);
    lv_obj_set_style_text_font(reader->body, app_ui_chinese_font_size(reader->font_px), 0);
    app_ui_label_fmt(reader->font_label, "%u号字", (unsigned)reader->font_px);
    app_ui_label_text(reader->theme_label, reader->dark ? "日间" : "夜间");
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
    const bool mounted = reader_storage_ready(reader->ui);
    if (reader->offset > reader->text_start_offset && mounted)
        lv_obj_remove_state(reader->previous, LV_STATE_DISABLED);
    else lv_obj_add_state(reader->previous, LV_STATE_DISABLED);
    if (reader->next_offset < reader->file_size && mounted)
        lv_obj_remove_state(reader->next, LV_STATE_DISABLED);
    else lv_obj_add_state(reader->next, LV_STATE_DISABLED);
}

static uint64_t source_position(const app_io_result_t *result, size_t utf8_byte)
{
    return result->source_offsets ? result->source_offsets[utf8_byte] : result->offset + utf8_byte;
}

static void render_page(reader_t *reader, const app_io_result_t *result, size_t relative)
{
    const char *text = result->text ? result->text : "";
    const size_t length = strlen(text);
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
    if (relative + page.consumed == length && result->next_offset > reader->next_offset)
        reader->next_offset = result->next_offset;
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
    update_position(reader);
    mark_dirty(reader);
}

static void page_loaded(app_ui_t *ui, const app_io_result_t *result)
{
    reader_t *reader = get_reader(ui);
    if (!reader || !reader->reading) return;
    reader->loading = false;
    if (result->error != ESP_OK) {
        app_ui_label_fmt(reader->position, "读取失败：%s，插卡后可重试", esp_err_to_name(result->error));
        return;
    }
    if (reader->request_kind == READ_RESUME && result->file_size &&
        reader->offset > result->file_size) {
        reader->history_count = 0;
        request_page(reader, 0, READ_SEEK);
        return;
    }
    if (reader->request_kind == READ_BACK_SCAN && result->text) {
        if (result->next_offset < reader->before_offset) {
            app_ui_label_text(reader->position, "回扫数据不足，请重新选择阅读位置");
            return;
        }
        const size_t length = strlen(result->text);
        size_t relative = 0;
        while (relative < length && source_position(result, relative) < reader->before_offset) {
            const app_reader_page_t page = layout(reader, result->text + relative, length - relative);
            if (!page.consumed || source_position(result, relative + page.consumed) >= reader->before_offset) break;
            relative += page.consumed;
        }
        render_page(reader, result, relative);
    } else {
        render_page(reader, result, 0);
    }
}

static void request_page(reader_t *reader, uint64_t offset, reader_request_kind_t kind)
{
    if (!reader || reader->loading) return;
    if (!reader_storage_ready(reader->ui)) {
        app_ui_label_text(reader->position, "TF 卡未挂载，请插卡后重试");
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

static void previous_event(lv_event_t *event) { previous_page(get_reader(lv_event_get_user_data(event))); }
static void next_event(lv_event_t *event) { next_page(get_reader(lv_event_get_user_data(event))); }

static void font_event(lv_event_t *event)
{
    reader_t *reader = get_reader(lv_event_get_user_data(event));
    if (!reader || reader->loading) return;
    if (!reader_storage_ready(reader->ui)) {
        app_ui_label_text(reader->position, "插卡后可以切换字体");
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
    reader->dark = !reader->dark;
    apply_theme(reader);
    mark_dirty(reader);
}

static void encoding_event(lv_event_t *event)
{
    reader_t *reader = get_reader(lv_event_get_user_data(event));
    if (!reader || reader->loading) return;
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

static void gesture_event(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    reader_t *reader = get_reader(lv_event_get_user_data(event));
    const lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (direction == LV_DIR_LEFT) next_page(reader);
    else if (direction == LV_DIR_RIGHT) previous_page(reader);
}

static void build_reader(reader_t *reader)
{
    lv_obj_t *content = reader->ui->content;
    lv_obj_clean(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(content, 12, 0);
    reader->title = app_ui_add_text(content, file_name(reader->path),
                                      LV_ALIGN_TOP_LEFT, 0, 0, 0xD8E2EC,
                                      app_ui_chinese_font_size(16));
    lv_obj_set_size(reader->title, READER_WIDTH - 108, 24);
    lv_label_set_long_mode(reader->title, LV_LABEL_LONG_DOT);
    lv_obj_t *encoding = reader_button(reader, content, "自动编码", READER_WIDTH - 104, 0, 104, encoding_event);
    lv_obj_set_height(encoding, 26);
    reader->encoding_label = lv_obj_get_child(encoding, 0);
    reader->body = app_ui_add_text(content, "正在恢复阅读位置…",
                                     LV_ALIGN_TOP_LEFT, 0, 30, 0xD8E2EC,
                                     app_ui_chinese_font_size(reader->font_px));
    lv_obj_set_size(reader->body, READER_WIDTH, READER_HEIGHT);
    lv_label_set_long_mode(reader->body, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_line_space(reader->body, READER_LINE_SPACE, 0);
    lv_obj_set_style_text_letter_space(reader->body, 0, 0);
    lv_obj_add_flag(reader->body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(reader->body, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(reader->body, gesture_event, LV_EVENT_GESTURE, reader->ui);
    reader->position = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 294,
                                         0x91ABBD, app_ui_chinese_font_size(16));
    lv_obj_set_width(reader->position, READER_WIDTH);
    reader->seek = lv_slider_create(content);
    lv_obj_set_pos(reader->seek, 5, 318);
    lv_obj_set_size(reader->seek, READER_WIDTH - 10U, 6);
    lv_slider_set_range(reader->seek, 0, 1000);
    lv_obj_add_event_cb(reader->seek, seek_event, LV_EVENT_RELEASED, reader->ui);
    reader->previous = reader_button(reader, content, "上一页", 0, 332, 84, previous_event);
    reader->next = reader_button(reader, content, "下一页", 88, 332, 84, next_event);
    lv_obj_t *button = reader_button(reader, content, "24号字", 176, 332, 78, font_event);
    reader->font_label = lv_obj_get_child(button, 0);
    button = reader_button(reader, content, "日间", 258, 332, 78, theme_event);
    reader->theme_label = lv_obj_get_child(button, 0);
    reader_button(reader, content, "书架", 340, 332, 84, bookshelf_event);
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
    }
    apply_theme(reader);
    static const char *const captions[] = {"自动编码", "UTF-8", "GBK", "UTF-16LE", "UTF-16BE"};
    app_ui_label_text(reader->encoding_label, captions[reader->encoding_hint]);
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
    build_reader(reader);
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
    reader->card_was_mounted = reader_storage_ready(ui);
    strlcpy(reader->path, selected, sizeof(reader->path));
    ui->page_context = reader;
    ui->page_cleanup = reader_cleanup;
    build_reader(reader);
    app_ui_set_timer(ui, reader_timer, 500);
    app_io_request_t request = {.operation = APP_IO_READER_LOAD};
    strlcpy(request.path, selected, sizeof(request.path));
    reader->loading = true;
    app_ui_request_io_ex(ui, &request, bookmark_loaded);
}

static void shelf_previous(lv_event_t *event)
{
    reader_t *reader = get_reader(lv_event_get_user_data(event));
    if (reader && reader->shelf_page) { --reader->shelf_page; show_shelf(reader); }
}

static void shelf_next(lv_event_t *event)
{
    reader_t *reader = get_reader(lv_event_get_user_data(event));
    if (reader && (reader->shelf_page + 1U) * SHELF_PAGE_SIZE < reader->book_count) {
        ++reader->shelf_page;
        show_shelf(reader);
    }
}

static void show_shelf(reader_t *reader)
{
    lv_obj_t *content = reader->ui->content;
    lv_obj_clean(content);
    lv_obj_set_style_pad_all(content, 12, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *hint = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 0,
                                     0x8FA9CD, app_ui_chinese_font_size(16));
    app_ui_label_fmt(hint, "UTF-8 TXT · 共 %u 本%s", (unsigned)reader->book_count,
                      reader->truncated ? "（列表已达上限）" : "");
    const size_t begin = reader->shelf_page * SHELF_PAGE_SIZE;
    size_t end = begin + SHELF_PAGE_SIZE;
    if (end > reader->book_count) end = reader->book_count;
    for (size_t index = begin; index < end; ++index) {
        lv_obj_t *button = lv_button_create(content);
        lv_obj_set_pos(button, 0, 30 + (int)(index - begin) * 46);
        lv_obj_set_size(button, READER_WIDTH, 42);
        lv_obj_set_style_radius(button, 10, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x172E39), 0);
        lv_obj_set_style_pad_all(button, 8, 0);
        lv_obj_set_user_data(button, (void *)(uintptr_t)index);
        lv_obj_add_event_cb(button, book_event, LV_EVENT_CLICKED, reader->ui);
        lv_obj_t *name = app_ui_add_text(button, file_name(reader->books[index].path),
                                         LV_ALIGN_LEFT_MID, 0, 0, 0xE6EDF8,
                                         app_ui_chinese_font_size(16));
        lv_obj_set_width(name, 320);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_t *size = app_ui_add_text(button, "", LV_ALIGN_RIGHT_MID, 0, 0,
                                         0x8FA9CD, app_ui_chinese_font_size(16));
        app_ui_label_fmt(size, "%" PRIu64 "K", (reader->books[index].size + 1023U) / 1024U);
    }
    if (!reader->book_count) {
        lv_obj_t *empty = app_ui_add_text(content,
            "没有找到小说\n\n请将 UTF-8 编码的 TXT 文件\n放入 /sdcard/Books/ 或卡根目录\n\n插卡后会自动刷新",
            LV_ALIGN_TOP_LEFT, 8, 70, 0xC9DCF8, app_ui_chinese_font_size(16));
        lv_obj_set_width(empty, 400);
        lv_obj_set_style_text_line_space(empty, 8, 0);
    }
    reader->previous = reader_button(reader, content, "上一组", 0, 330, 100, shelf_previous);
    reader_button(reader, content, "刷新书架", 162, 330, 100, bookshelf_event);
    reader->next = reader_button(reader, content, "下一组", 324, 330, 100, shelf_next);
    if (!reader->shelf_page) lv_obj_add_state(reader->previous, LV_STATE_DISABLED);
    if (end >= reader->book_count) lv_obj_add_state(reader->next, LV_STATE_DISABLED);
    reader->position = app_ui_add_text(content, "", LV_ALIGN_TOP_MID, 0, 305,
                                         0x91ABBD, app_ui_chinese_font_size(16));
    app_ui_label_fmt(reader->position, "%u / %u", (unsigned)(reader->shelf_page + 1U),
                      (unsigned)(reader->book_count ?
                          (reader->book_count + SHELF_PAGE_SIZE - 1U) / SHELF_PAGE_SIZE : 1U));
}

static void books_loaded(app_ui_t *ui, const app_io_result_t *result)
{
    reader_t *reader = get_reader(ui);
    if (!reader || reader->reading) return;
    reader->loading = false;
    free(reader->books);
    reader->books = NULL;
    reader->book_count = 0;
    reader->shelf_page = 0;
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
    if (reader->dirty && esp_timer_get_time() >= reader->save_at_us) save_bookmark(reader);
    const esp_err_t save_error = app_io_bookmark_status(ui->io);
    const bool pending = app_io_bookmark_pending(ui->io);
    if (reader->reading && (save_error != reader->last_save_error || pending != reader->bookmark_pending)) {
        reader->last_save_error = save_error;
        reader->bookmark_pending = pending;
        if (save_error != ESP_OK) app_ui_label_text(reader->position, "书签保存失败，后台重试中");
        else if (!reader->loading) update_position(reader);
    }
    const bool mounted = reader_storage_ready(ui);
    if (reader->card_was_mounted && !mounted) {
        reader->reload_after_insert = true;
        save_bookmark(reader);
        if (reader->position) app_ui_label_text(reader->position, "TF 卡已拔出，阅读位置已保留");
        if (reader->previous) lv_obj_add_state(reader->previous, LV_STATE_DISABLED);
        if (reader->next) lv_obj_add_state(reader->next, LV_STATE_DISABLED);
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
    reader->card_was_mounted = reader_storage_ready(ui);
    ui->page_context = reader;
    ui->page_cleanup = reader_cleanup;
    app_ui_add_text(content, "正在扫描书架…", LV_ALIGN_TOP_LEFT, 0, 0,
                      0xC9DCF8, app_ui_chinese_font_size(16));
    app_ui_set_timer(ui, reader_timer, 500);
    request_books(reader);
}
