#include "aether_media_fs.h"
#include "app_ui_internal.h"
#include "app_text.h"
#include "ui/app_scroll_list.h"
#include "../assets/icons/app_icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"

#define FILES_LIMIT 4096U
typedef struct {
    char directory[192];
    app_io_entry_t *entries;
    size_t count;
    app_scroll_list_t rows;
    uint32_t media_generation;
    bool mounted, loading, retry_allowed;
    uint32_t retry_after;
    lv_obj_t *path, *info, *up, *refresh, *source;
} files_page_t;

static const char *filename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool files_storage_ready(app_ui_t *ui)
{
    files_page_t *page = ui->page_context;
    return page && aether_media_fs_path_ready(ui->storage, page->directory) &&
        !app_io_usb_storage_blocked(ui->io);
}
static void files_refresh(app_ui_t *ui);

static void enabled(lv_obj_t *object, bool value)
{
    if (value) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static void files_controls(files_page_t *page)
{
    const bool ready = page->mounted && !page->loading;
    if (page->source) enabled(page->source, !page->loading);
    const char *root = !strncmp(page->directory, "/usb", 4) ? "/usb" : AETHER_STORAGE_MOUNT_POINT;
    enabled(page->up, ready && strcmp(page->directory, root));
    enabled(page->refresh, ready || (!page->loading && page->retry_allowed));
    lv_obj_set_style_opa(page->rows.viewport, page->loading ? LV_OPA_50 : LV_OPA_COVER, 0);
}

static void files_unavailable(app_ui_t *ui, files_page_t *page)
{
    page->retry_allowed = false;
    if (app_io_usb_storage_blocked(ui->io)) {
        app_ui_label_text(page->info,"存储设备由电脑占用，请先退出 USB 传输");
        return;
    }
    if (!strncmp(page->directory,"/usb",4)) {
        app_ui_label_text(page->info,"USB U盘未就绪；接入后会自动刷新");
        return;
    }
    aether_storage_status_t status = {0};
    if (aether_storage_get_status(ui->storage,&status) != ESP_OK || !status.card_inserted) {
        app_ui_label_text(page->info,"存储设备已移除；重新接入后会自动刷新");
        return;
    }
    page->retry_allowed = (int32_t)(lv_tick_get()-page->retry_after) >= 0;
    aether_storage_diagnostics_t diag = {0};
    if (aether_storage_get_diagnostics(ui->storage,&diag) == ESP_OK && diag.attempted)
        app_ui_label_fmt(page->info,"TF 挂载失败：%s / %s\n点击刷新重试；不会格式化内存卡",
            aether_storage_fs_name(diag.filesystem),aether_storage_layout_name(diag.layout));
    else app_ui_label_text(page->info,"TF 卡正在准备；可点刷新重试");
}

static const lv_image_dsc_t *file_icon(const app_io_entry_t *entry)
{
    const char *ext = strrchr(entry->path, '.');
    if (entry->directory) return &icon_files;
    if (ext && (!strcasecmp(ext, ".mp3") || !strcasecmp(ext, ".m4a") || !strcasecmp(ext, ".wav")))
        return &icon_music;
    if (ext && !strcasecmp(ext, ".txt")) return &icon_reader;
    if (ext && !strcasecmp(ext, ".nes")) return &icon_nes;
    if (ext && (!strcasecmp(ext, ".mp4") || !strcasecmp(ext, ".m4v") || !strcasecmp(ext, ".avi") ||
                !strcasecmp(ext, ".mjpg") || !strcasecmp(ext, ".mjpeg"))) return &icon_video;
    return NULL;
}

static void files_cleanup(app_ui_t *ui)
{
    files_page_t *page = ui->page_context;
    if (page) {
        app_scroll_list_deinit(&page->rows);
        free(page->entries);
        free(page);
    }
    ui->page_context = NULL;
}

static void files_clear_rows(files_page_t *page)
{
    app_scroll_list_set_count(&page->rows, 0);
    free(page->entries);
    page->entries = NULL;
    page->count = 0;
}

static void file_clicked(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    files_page_t *page = ui->page_context;
    const size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (!page || index >= page->count || page->loading || ui->io_complete) return;
    if (aether_media_fs_generation(ui->storage, page->directory) != page->media_generation) {
        files_refresh(ui); return;
    }
    app_io_entry_t item = page->entries[index];
    if (item.directory) {
        strlcpy(page->directory, item.path, sizeof(page->directory));
        files_clear_rows(page);
        files_refresh(ui);
        return;
    }
    const char *ext = strrchr(item.path, '.');
    if (ext && !strcasecmp(ext, ".txt")) app_open_reader_path(ui, item.path);
    else if (ext && (!strcasecmp(ext, ".wav") || !strcasecmp(ext, ".mp3") || !strcasecmp(ext, ".m4a"))) app_open_music_path(ui, item.path);
    else if (ext && (!strcasecmp(ext, ".mp4") || !strcasecmp(ext, ".m4v") || !strcasecmp(ext, ".avi") || !strcasecmp(ext, ".mjpg") || !strcasecmp(ext, ".mjpeg"))) app_open_video_path(ui, item.path);
    else if (ext && !strcasecmp(ext, ".nes")) app_open_nes_path(ui, item.path);
    else app_ui_label_fmt(page->info, "文件大小 %llu 字节；此格式暂不支持打开", (unsigned long long)item.size);
}

static lv_obj_t *create_file_row(lv_obj_t *parent, void *context)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, 396, 66);
    lv_obj_set_style_pad_all(row, 9, 0);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x18243A), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2B405E), LV_STATE_PRESSED);
    lv_obj_t *image = lv_image_create(row);
    lv_obj_align(image, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(image, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(image, lv_color_hex(0xACC1DD), 0);
    lv_obj_t *title = app_ui_add_text(row, "", LV_ALIGN_TOP_LEFT, 60, 0,
                                     0xEAF2FF, app_ui_chinese_font());
    lv_obj_set_size(title, 310, 21);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t *metadata = app_ui_add_text(row, "", LV_ALIGN_TOP_LEFT, 60, 26,
                                        0x91A8C7, app_ui_chinese_font());
    lv_obj_set_size(metadata, 310, 20);
    lv_label_set_long_mode(metadata, LV_LABEL_LONG_DOT);
    lv_obj_add_event_cb(row, file_clicked, LV_EVENT_CLICKED, context);
    return row;
}

static void bind_file_row(lv_obj_t *row, size_t index, void *context)
{
    const app_ui_t *ui = context;
    const files_page_t *page = ui->page_context;
    const app_io_entry_t *entry = &page->entries[index];
    char name[sizeof(entry->path)];
    strlcpy(name, filename(entry->path), sizeof(name));
    app_text_sanitize_utf8(name, strlen(name));
    const lv_image_dsc_t *icon = file_icon(entry);
    lv_image_set_src(lv_obj_get_child(row, 0), icon ? (const void *)icon : (const void *)LV_SYMBOL_FILE);
    app_ui_label_text(lv_obj_get_child(row, 1), name);
    lv_obj_t *metadata = lv_obj_get_child(row, 2);
    const unsigned long long bytes = entry->size;
    if (entry->directory) app_ui_label_text(metadata, "文件夹");
    else if (bytes >= 1024U * 1024U) app_ui_label_fmt(metadata, "%llu.%llu MiB",
        bytes / (1024U * 1024U), (bytes % (1024U * 1024U)) * 10U / (1024U * 1024U));
    else if (bytes >= 1024U) app_ui_label_fmt(metadata, "%llu.%llu KiB",
        bytes / 1024U, (bytes % 1024U) * 10U / 1024U);
    else app_ui_label_fmt(metadata, "%llu B", bytes);
}

static void files_loaded(app_ui_t *ui, const app_io_result_t *result)
{
    files_page_t *page = ui->page_context;
    if (!page) return;
    page->loading = false;
    if (aether_media_fs_generation(ui->storage, page->directory) != page->media_generation) {
        files_refresh(ui); return;
    }
    page->mounted = files_storage_ready(ui);
    files_clear_rows(page);
    if (!page->mounted) {
        files_unavailable(ui,page);
        files_controls(page);
        return;
    }
    if (result->error != ESP_OK) {
        app_ui_label_fmt(page->info, "读取失败：%s；设备就绪后点刷新重试", esp_err_to_name(result->error));
        files_controls(page);
        return;
    }
    if (result->count) page->entries = heap_caps_malloc(result->count * sizeof(*page->entries),
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result->count && !page->entries) {
        app_ui_label_text(page->info, "内存不足，无法显示目录");
        files_controls(page);
        return;
    }
    if (result->count) memcpy(page->entries, result->entries, result->count * sizeof(*page->entries));
    page->count = result->count;
    app_scroll_list_set_count(&page->rows, page->count);
    app_ui_label_fmt(page->info, "%u 项%s", (unsigned)result->total_count,
        result->truncated ? " · 部分项目未索引" : "");
    if (!page->count) app_ui_label_text(page->info, "这个文件夹为空");
    files_controls(page);
}

static void files_refresh(app_ui_t *ui)
{
    files_page_t *page = ui->page_context;
    app_ui_label_text(page->path, page->directory);
    page->media_generation = aether_media_fs_generation(ui->storage, page->directory);
    page->mounted = files_storage_ready(ui);
    page->retry_allowed = false;
    if (!page->mounted) {
        files_clear_rows(page);
        page->loading = false;
        files_unavailable(ui,page);
        files_controls(page);
        return;
    }
    page->loading = true;
    app_ui_label_text(page->info, "正在读取目录…");
    files_controls(page);
    app_io_request_t request = {.operation = APP_IO_CATALOG, .filter = APP_IO_FILES, .limit = FILES_LIMIT};
    strlcpy(request.path, page->directory, sizeof(request.path));
    app_ui_request_io_ex(ui, &request, files_loaded);
}
static void files_action(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    files_page_t *page = ui->page_context;
    if (!page || ui->io_complete) return;
    intptr_t action = (intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (!action && !page->mounted && page->retry_allowed && strncmp(page->directory,"/usb",4)) {
        const esp_err_t error = aether_storage_request_retry(ui->storage);
        if (error != ESP_OK) { app_ui_label_fmt(page->info,"TF 重试失败：%s",esp_err_to_name(error));return; }
        page->retry_after = lv_tick_get()+1000;
    }
    if (action == 1) {
        char *slash = strrchr(page->directory, '/');
        if (slash && slash > page->directory) *slash = 0;
        files_clear_rows(page);
    }
    files_refresh(ui);
}
static lv_obj_t *files_button(app_ui_t *ui, const char *text, int x, int y, intptr_t action)
{
    lv_obj_t *button = lv_button_create(ui->content);
    lv_obj_set_pos(button, x, y); lv_obj_set_size(button, 54, 44);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x20334F), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x314C70), LV_STATE_PRESSED);
    lv_obj_set_user_data(button, (void *)action);
    lv_obj_add_event_cb(button, files_action, LV_EVENT_CLICKED, ui);
    app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, &lv_font_montserrat_24);
    return button;
}
static void files_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    files_page_t *page = ui->page_context;
    if (!page) return;
    const bool ready = files_storage_ready(ui);
    const uint32_t generation = aether_media_fs_generation(ui->storage, page->directory);
    if (ready != page->mounted || generation != page->media_generation) {
        page->media_generation = generation;
        page->mounted = ready;
        if (ready) files_refresh(ui);
        else {
            files_clear_rows(page);
            page->loading = false;
        }
    }
    if (!ready && !page->loading) { files_unavailable(ui,page); files_controls(page); }
}
static void files_source(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    files_page_t *page = ui->page_context;
    if (!page || ui->io_complete) return;
    strlcpy(page->directory, lv_dropdown_get_selected(page->source) ? "/usb" : AETHER_STORAGE_MOUNT_POINT,
            sizeof(page->directory));
    files_clear_rows(page);
    files_refresh(ui);
}
void app_open_files_path(app_ui_t *ui, const char *path)
{
    char selected[192];
    const char *source = path && strlen(path) < sizeof(selected) && aether_media_fs_safe_path(path) ?
        path : aether_media_fs_default_root(ui->storage);
    strlcpy(selected, source, sizeof(selected));
    lv_obj_t *content = app_ui_begin_page(ui, "文件浏览");
    files_page_t *page = calloc(1, sizeof(*page));
    if (!page) { app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF8888, NULL); return; }
    ui->page_context = page; ui->page_cleanup = files_cleanup;
    strlcpy(page->directory, selected, sizeof(page->directory));
    page->mounted = files_storage_ready(ui);
    page->path = app_ui_add_text(content, page->directory, LV_ALIGN_TOP_LEFT, 0, 0, 0x77B8FF, NULL);
    lv_obj_set_width(page->path, 248); lv_label_set_long_mode(page->path, LV_LABEL_LONG_DOT);
    page->source = lv_dropdown_create(content);
    lv_obj_set_pos(page->source, 258, -8); lv_obj_set_size(page->source, 146, 32);
    lv_obj_set_style_text_font(page->source, app_ui_chinese_font(), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(page->source), app_ui_chinese_font(), 0);
    lv_obj_set_style_bg_color(page->source, lv_color_hex(0x20334F), 0);
    lv_obj_set_style_text_color(page->source, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_border_width(page->source, 0, 0);
    lv_obj_set_style_bg_color(lv_dropdown_get_list(page->source), lv_color_hex(0x18243A), 0);
    lv_obj_set_style_text_color(lv_dropdown_get_list(page->source), lv_color_hex(0xEAF2FF), 0);
    lv_dropdown_set_options(page->source, "TF 卡\nUSB U盘");
    lv_dropdown_set_selected(page->source, !strncmp(page->directory, "/usb", 4));
    lv_obj_add_event_cb(page->source, files_source, LV_EVENT_VALUE_CHANGED, ui);
    page->up = files_button(ui, LV_SYMBOL_UP, 0, 30, 1);
    page->refresh = files_button(ui, LV_SYMBOL_REFRESH, 64, 30, 0);
    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_pos(list, 0, 84);
    lv_obj_set_size(list, 408, 240);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    app_scroll_list_init(&page->rows, list, 72, create_file_row, bind_file_row, ui);
    page->info = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 332, 0xAFC6E9, NULL);
    lv_obj_set_width(page->info, 405);
    app_ui_set_timer(ui, files_timer, 500); files_refresh(ui);
}

void app_open_files(app_ui_t *ui)
{
    app_open_files_path(ui, NULL);
}
