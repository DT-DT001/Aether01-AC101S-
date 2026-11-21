#include "aether_media_fs.h"
#include "app_ui_internal.h"
#include "app_text.h"
#include "../assets/icons/app_icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FILES_PAGE 32U
typedef struct {
    char directory[192];
    app_io_entry_t *entries;
    size_t count, total;
    uint64_t offset;
    uint32_t media_generation;
    bool mounted, loading, retry_allowed;
    uint32_t retry_after;
    lv_obj_t *path, *list, *info, *up, *refresh, *previous, *next, *source;
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
    enabled(page->previous, ready && page->offset);
    enabled(page->next, ready && page->offset + page->count < page->total &&
            page->offset + page->count < 4096);
    lv_obj_set_style_opa(page->list, page->loading ? LV_OPA_50 : LV_OPA_COVER, 0);
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

static const lv_image_dsc_t *file_icon(const app_io_entry_t *entry, const char **kind)
{
    const char *ext = strrchr(entry->path, '.');
    if (entry->directory) { *kind = "文件夹"; return &icon_files; }
    if (ext && (!strcasecmp(ext, ".mp3") || !strcasecmp(ext, ".m4a") || !strcasecmp(ext, ".wav"))) {
        *kind = "音频"; return &icon_music;
    }
    if (ext && !strcasecmp(ext, ".txt")) { *kind = "文本"; return &icon_reader; }
    if (ext && !strcasecmp(ext, ".nes")) { *kind = "游戏"; return &icon_nes; }
    if (ext && (!strcasecmp(ext, ".mp4") || !strcasecmp(ext, ".m4v") || !strcasecmp(ext, ".avi") ||
                !strcasecmp(ext, ".mjpg") || !strcasecmp(ext, ".mjpeg"))) {
        *kind = "视频"; return &icon_video;
    }
    *kind = "文件";
    return NULL;
}

static void files_cleanup(app_ui_t *ui)
{
    files_page_t *page = ui->page_context;
    if (page) { free(page->entries); free(page); }
    ui->page_context = NULL;
}

static void file_clicked(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    files_page_t *page = ui->page_context;
    const size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (!page || index >= page->count || ui->io_complete) return;
    if (aether_media_fs_generation(ui->storage, page->directory) != page->media_generation) {
        files_refresh(ui); return;
    }
    app_io_entry_t item = page->entries[index];
    if (item.directory) {
        strlcpy(page->directory, item.path, sizeof(page->directory));
        page->offset = 0; files_refresh(ui); return;
    }
    const char *ext = strrchr(item.path, '.');
    if (ext && !strcasecmp(ext, ".txt")) app_open_reader_path(ui, item.path);
    else if (ext && (!strcasecmp(ext, ".wav") || !strcasecmp(ext, ".mp3") || !strcasecmp(ext, ".m4a"))) app_open_music_path(ui, item.path);
    else if (ext && (!strcasecmp(ext, ".mp4") || !strcasecmp(ext, ".m4v") || !strcasecmp(ext, ".avi") || !strcasecmp(ext, ".mjpg") || !strcasecmp(ext, ".mjpeg"))) app_open_video_path(ui, item.path);
    else if (ext && !strcasecmp(ext, ".nes")) app_open_nes_path(ui, item.path);
    else app_ui_label_fmt(page->info, "文件大小 %llu 字节；此格式暂不支持打开", (unsigned long long)item.size);
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
    free(page->entries); page->entries = NULL; page->count = page->total = 0;
    lv_obj_clean(page->list);
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
    page->entries = malloc(result->count * sizeof(*page->entries));
    if (result->count && !page->entries) {
        app_ui_label_text(page->info, "内存不足，无法显示目录");
        files_controls(page);
        return;
    }
    if (result->count) memcpy(page->entries, result->entries, result->count * sizeof(*page->entries));
    page->count = result->count; page->total = result->total_count;
    for (size_t i = 0; i < page->count; ++i) {
        char detail[96], name[192];
        strlcpy(name, filename(page->entries[i].path), sizeof(name));
        app_text_sanitize_utf8(name, strlen(name));
        const char *kind;
        const lv_image_dsc_t *icon = file_icon(&page->entries[i], &kind);
        lv_obj_t *button = lv_button_create(page->list);
        lv_obj_set_size(button, LV_PCT(100), 72);
        lv_obj_set_flex_grow(button, 0);
        lv_obj_set_style_pad_all(button, 10, 0);
        lv_obj_set_style_radius(button, 14, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x18243A), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x2B405E), LV_STATE_PRESSED);
        if (icon) {
            lv_obj_t *image = lv_image_create(button);
            lv_image_set_src(image, icon);
            lv_obj_align(image, LV_ALIGN_LEFT_MID, 0, 0);
        } else app_ui_add_text(button, LV_SYMBOL_FILE, LV_ALIGN_LEFT_MID, 12, 0,
                                0xACC1DD, &lv_font_montserrat_24);
        lv_obj_t *title = app_ui_add_text(button, name, LV_ALIGN_TOP_LEFT, 60, 0,
                                          0xEAF2FF, app_ui_chinese_font());
        lv_obj_set_size(title, 310, 20);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        const uint64_t bytes = page->entries[i].size;
        if (page->entries[i].directory) snprintf(detail, sizeof(detail), "文件夹 · 点击进入");
        else if (bytes >= 1024U * 1024U) snprintf(detail, sizeof(detail), "%s · %llu.%llu MiB", kind,
            (unsigned long long)(bytes / (1024U * 1024U)),
            (unsigned long long)((bytes % (1024U * 1024U)) * 10U / (1024U * 1024U)));
        else if (bytes >= 1024U) snprintf(detail, sizeof(detail), "%s · %llu.%llu KiB", kind,
            (unsigned long long)(bytes / 1024U), (unsigned long long)((bytes % 1024U) * 10U / 1024U));
        else snprintf(detail, sizeof(detail), "%s · %llu B", kind, (unsigned long long)bytes);
        lv_obj_t *metadata = app_ui_add_text(button, detail, LV_ALIGN_TOP_LEFT, 60, 28,
                                             0x91A8C7, app_ui_chinese_font());
        lv_obj_set_size(metadata, 310, 20);
        lv_label_set_long_mode(metadata, LV_LABEL_LONG_DOT);
        lv_obj_set_user_data(button, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(button, file_clicked, LV_EVENT_CLICKED, ui);
    }
    app_ui_label_fmt(page->info, "共 %u 项 · 第 %llu 页%s", (unsigned)page->total,
        (unsigned long long)(page->offset / FILES_PAGE + 1),
        result->total_count > 4096 ? "（目录过大，仅索引前4096项）" : "");
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
        page->loading = false;
        files_unavailable(ui,page);
        files_controls(page);
        return;
    }
    page->loading = true;
    app_ui_label_text(page->info, "正在读取目录…");
    files_controls(page);
    app_io_request_t request = {.operation = APP_IO_CATALOG, .filter = APP_IO_FILES,
                                .offset = page->offset, .limit = FILES_PAGE};
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
        page->offset = 0;
    } else if (action == 2 && page->offset >= FILES_PAGE) page->offset -= FILES_PAGE;
    else if (action == 3 && page->offset + page->count < page->total) page->offset += FILES_PAGE;
    files_refresh(ui);
}
static lv_obj_t *files_button(app_ui_t *ui, const char *text, int x, int y, intptr_t action)
{
    lv_obj_t *button = lv_button_create(ui->content);
    lv_obj_set_pos(button, x, y); lv_obj_set_size(button, 92, 44);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x20334F), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x314C70), LV_STATE_PRESSED);
    lv_obj_set_user_data(button, (void *)action);
    lv_obj_add_event_cb(button, files_action, LV_EVENT_CLICKED, ui);
    app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0, 0xFFFFFF, NULL);
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
            lv_obj_clean(page->list); free(page->entries); page->entries = NULL; page->count = 0;
            page->total = 0; page->loading = false;
            files_unavailable(ui,page);
            files_controls(page);
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
    page->offset = 0;
    lv_obj_clean(page->list);
    free(page->entries); page->entries = NULL; page->count = page->total = 0;
    files_refresh(ui);
}
void app_open_files_path(app_ui_t *ui, const char *path)
{
    lv_obj_t *content = app_ui_begin_page(ui, "文件浏览");
    files_page_t *page = calloc(1, sizeof(*page));
    if (!page) { app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF8888, NULL); return; }
    ui->page_context = page; ui->page_cleanup = files_cleanup;
    strlcpy(page->directory, path && aether_media_fs_safe_path(path) ? path : aether_media_fs_default_root(ui->storage),
             sizeof(page->directory));
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
    page->up = files_button(ui, "上级目录", 0, 30, 1);
    page->refresh = files_button(ui, "刷新", 104, 30, 0);
    page->previous = files_button(ui, "上一页", 208, 30, 2);
    page->next = files_button(ui, "下一页", 312, 30, 3);
    page->list = lv_list_create(content); lv_obj_set_pos(page->list, 0, 84); lv_obj_set_size(page->list, 408, 226);
    lv_obj_set_style_bg_opa(page->list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page->list, 0, 0);
    lv_obj_set_style_pad_all(page->list, 2, 0);
    lv_obj_set_style_pad_row(page->list, 6, 0);
    page->info = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 320, 0xAFC6E9, NULL);
    lv_obj_set_width(page->info, 405);
    app_ui_set_timer(ui, files_timer, 500); files_refresh(ui);
}

void app_open_files(app_ui_t *ui)
{
    app_open_files_path(ui, NULL);
}
