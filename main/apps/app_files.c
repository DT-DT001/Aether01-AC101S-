#include "aether_media_fs.h"
#include "app_ui_internal.h"
#include "app_catalog_ui.h"
#include "app_text.h"
#include "ui/app_scroll_list.h"
#include "ui/app_text_input.h"
#include "ui/app_ui_modal.h"
#include "../assets/icons/app_icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "esp_heap_caps.h"

#define FILES_LIMIT 4096U
typedef struct {
    char directory[192];
    size_t exit_path_length;
    void (*exit_back)(app_ui_t *ui);
    char clipboard[192], query[96], requested_query[96];
    app_io_entry_t selected;
    app_io_entry_t *entries;
    size_t count;
    app_scroll_list_t rows;
    uint32_t media_generation;
    uint32_t clipboard_generation;
    uint32_t restore_generation;
    int32_t restore_scroll_y;
    bool mounted, loading, retry_allowed;
    bool select_mode, cut, mutating;
    const char *feedback;
    uint32_t retry_after;
    lv_obj_t *path, *info, *refresh, *source, *quick, *search;
    lv_obj_t *dialog, *search_field, *search_notice;
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
static void files_open(app_ui_t *ui, const char *path, const app_files_return_t *previous);
static void files_search_start(app_ui_t *ui, const char *query);
static void files_folder_back(app_ui_t *ui);

static void files_update_back(app_ui_t *ui, const files_page_t *page)
{
    ui->page_back = strlen(page->directory) > page->exit_path_length ? files_folder_back : page->exit_back;
}

static void files_capture_entry_route(app_ui_t *ui, files_page_t *page)
{
    /* A caller can make the folder it opened the return point to its page. */
    if (ui->page_back == files_folder_back || ui->page_back == page->exit_back) return;
    page->exit_back = ui->page_back;
    page->exit_path_length = strlen(page->directory);
}

static void enabled(lv_obj_t *object, bool value)
{
    if (value) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static void files_controls(files_page_t *page)
{
    const bool busy = page->loading || page->mutating;
    const bool ready = page->mounted && !busy;
    enabled(page->source, !busy);
    enabled(page->refresh, ready || (!busy && page->retry_allowed));
    enabled(page->search, ready);
    enabled(page->quick, ready && (page->count != 0 || page->clipboard[0]));
    if (page->select_mode || page->clipboard[0]) lv_obj_add_state(page->quick, LV_STATE_CHECKED);
    else lv_obj_remove_state(page->quick, LV_STATE_CHECKED);
    app_ui_label_text(lv_obj_get_child(page->quick, 0), page->clipboard[0] ? LV_SYMBOL_PASTE : LV_SYMBOL_LIST);
    app_ui_label_text(lv_obj_get_child(page->refresh, 0), page->query[0] ? LV_SYMBOL_CLOSE : LV_SYMBOL_REFRESH);
    lv_obj_set_style_opa(page->rows.viewport, busy ? LV_OPA_50 : LV_OPA_COVER, 0);
}

static void files_unavailable(app_ui_t *ui, files_page_t *page)
{
    page->retry_allowed = false;
    if (app_io_usb_storage_blocked(ui->io)) {
        app_ui_label_text(page->info,"存储设备由电脑占用，请先退出 USB 传输");
        return;
    }
    if (!strncmp(page->directory,"/usb",4)) {
        app_ui_label_text(page->info,"USB U盘未就绪");
        return;
    }
    aether_storage_status_t status = {0};
    if (aether_storage_get_status(ui->storage,&status) != ESP_OK || !status.card_inserted) {
        app_ui_label_text(page->info,"存储设备已移除");
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
        if (page->dialog) lv_msgbox_close(page->dialog);
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

static void files_folder_back(app_ui_t *ui)
{
    files_page_t *page = ui->page_context;
    if (!page || page->loading || page->mutating || page->dialog || ui->io_complete) return;
    char *slash = strrchr(page->directory, '/');
    if (!slash || slash == page->directory) return;
    *slash = '\0';
    page->restore_scroll_y = 0;
    page->feedback = NULL;
    files_update_back(ui, page);
    files_clear_rows(page);
    files_refresh(ui);
}

static void files_back_from_audio(app_ui_t *ui)
{
    const app_files_return_t previous = ui->files_return;
    files_open(ui, previous.directory, &previous);
}

static void files_mutation_done(app_ui_t *ui, const app_io_result_t *result)
{
    files_page_t *page = ui->page_context;
    if (!page || !page->mutating) return;
    page->mutating = false;
    const bool deleting = result->operation == APP_IO_DELETE_FILE;
    const bool moving = result->operation == APP_IO_MOVE_FILE;
    if (result->target_exists) page->feedback = "目标已有同名文件";
    else if (result->source_retained) page->feedback = "已复制，原文件仍保留";
    else if (result->error == ESP_ERR_NOT_FOUND) page->feedback = "文件已不存在";
    else if (result->error == ESP_ERR_INVALID_STATE) page->feedback = "存储状态已变化，请重试";
    else if (result->error != ESP_OK) page->feedback = deleting ? "删除失败" : moving ? "移动失败" : "复制失败";
    else page->feedback = deleting ? "已删除文件" : moving ? "已移动文件" : "已复制文件";
    if ((moving && (result->error == ESP_OK || result->source_retained)) ||
        (!deleting && result->error == ESP_ERR_NOT_FOUND) ||
        (deleting && result->error == ESP_OK && !strcmp(result->path, page->clipboard))) page->clipboard[0] = '\0';

    const bool same_media = files_storage_ready(ui) &&
        aether_media_fs_generation(ui->storage, page->directory) == page->media_generation;
    if (!same_media || result->error == ESP_OK || result->source_retained || result->error == ESP_ERR_NOT_FOUND) {
        page->restore_scroll_y = lv_obj_get_scroll_y(page->rows.viewport);
        page->restore_generation = page->media_generation;
        if (same_media && page->query[0]) files_search_start(ui, page->query);
        else files_refresh(ui);
    } else {
        app_ui_label_text(page->info, page->feedback);
        files_controls(page);
    }
}

static void files_submit_mutation(app_ui_t *ui, files_page_t *page, const app_io_request_t *request)
{
    page->mutating = true;
    page->select_mode = false;
    app_ui_label_text(page->info, request->operation == APP_IO_DELETE_FILE ? "正在删除…" :
        request->operation == APP_IO_MOVE_FILE ? "正在移动…" : "正在复制…");
    files_controls(page);
    app_ui_request_io_ex(ui, request, files_mutation_done);
}

static void files_paste(app_ui_t *ui, files_page_t *page)
{
    if (!aether_media_fs_path_ready(ui->storage, page->clipboard) ||
        aether_media_fs_generation(ui->storage, page->clipboard) != page->clipboard_generation) {
        page->clipboard[0] = '\0';
        app_ui_label_text(page->info, "源存储已移除，请重新选择文件");
        files_controls(page);
        return;
    }
    app_io_request_t request = {.operation = page->cut ? APP_IO_MOVE_FILE : APP_IO_COPY_FILE,
        .media_generation = page->clipboard_generation, .destination_generation = page->media_generation};
    strlcpy(request.path, page->clipboard, sizeof(request.path));
    const int length = snprintf(request.destination, sizeof(request.destination), "%s/%s",
                                page->directory, filename(page->clipboard));
    if (length < 0 || (size_t)length >= sizeof(request.destination)) {
        app_ui_label_text(page->info, "目标路径过长");
        return;
    }
    if (!strcasecmp(request.path, request.destination)) {
        app_ui_label_text(page->info, "文件已在此处，请选择其他文件夹");
        return;
    }
    files_submit_mutation(ui, page, &request);
}

static void files_dialog_closed(lv_event_t *event)
{
    files_page_t *page = lv_event_get_user_data(event);
    page->dialog = page->search_field = page->search_notice = NULL;
}

static void files_dialog_cancel(lv_event_t *event)
{
    files_page_t *page = lv_event_get_user_data(event);
    if (page->dialog) lv_msgbox_close_async(page->dialog);
}

static lv_obj_t *files_dialog(app_ui_t *ui, files_page_t *page, const char *title)
{
    lv_obj_t *box = app_ui_modal_msgbox(ui->screen);
    if (!box) return NULL;
    lv_obj_t *backdrop = lv_obj_get_parent(box);
    lv_obj_set_style_bg_color(backdrop, lv_color_hex(0x060912), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_70, 0);
    page->dialog = box;
    lv_obj_stop_scroll_anim(page->rows.viewport);
    lv_obj_set_width(box, 374);
    lv_obj_set_style_text_font(box, app_ui_chinese_font(), 0);
    lv_obj_set_style_text_color(box, lv_color_hex(0xEAF2FF), 0);
    app_catalog_panel(box);
    lv_obj_set_style_radius(box, 20, 0);
    lv_obj_add_event_cb(box, files_dialog_closed, LV_EVENT_DELETE, page);
    lv_msgbox_add_title(box, title);
    lv_obj_set_style_bg_color(lv_msgbox_get_header(box), lv_color_hex(0x1D2D45), 0);
    lv_obj_set_style_text_color(lv_msgbox_get_title(box), lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_text_font(lv_msgbox_get_title(box), app_ui_chinese_font_size(24), 0);
    return box;
}

static lv_obj_t *files_dialog_button(lv_obj_t *box, const char *caption, bool warning,
                                     lv_event_cb_t callback, void *context)
{
    lv_obj_t *button = lv_msgbox_add_footer_button(box, caption);
    lv_obj_set_size(button, 128, 44);
    lv_obj_set_height(lv_msgbox_get_footer(box), 56);
    lv_obj_set_style_pad_bottom(lv_msgbox_get_footer(box), 8, 0);
    app_catalog_control(button, warning ? 0x71394D : 0x20344C);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, context);
    return button;
}

static void delete_confirm(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    files_page_t *page = ui->page_context;
    if (!page || !page->dialog || page->mutating || ui->io_complete) return;
    app_io_request_t request = {.operation = APP_IO_DELETE_FILE, .media_generation = page->media_generation};
    strlcpy(request.path, page->selected.path, sizeof(request.path));
    lv_msgbox_close(page->dialog);
    if (!files_storage_ready(ui) ||
        aether_media_fs_generation(ui->storage, request.path) != request.media_generation) {
        files_refresh(ui);
        return;
    }
    files_submit_mutation(ui, page, &request);
}

static void files_confirm_delete(app_ui_t *ui, files_page_t *page)
{
    lv_obj_t *box = files_dialog(ui, page, "删除文件");
    if (!box) return;
    char name[sizeof(page->selected.path)];
    strlcpy(name, filename(page->selected.path), sizeof(name));
    app_text_sanitize_utf8(name, strlen(name));
    lv_msgbox_add_text(box, name);
    lv_obj_t *cancel = files_dialog_button(box, "取消", false, files_dialog_cancel, page);
    files_dialog_button(box, "确认删除", true, delete_confirm, ui);
    app_ui_modal_focus(box, cancel);
}

static void files_show_details(app_ui_t *ui, files_page_t *page)
{
    lv_obj_t *box = files_dialog(ui, page, "详细信息");
    if (!box) return;
    char path[sizeof(page->selected.path)];
    strlcpy(path, page->selected.path, sizeof(path));
    app_text_sanitize_utf8(path, strlen(path));
    lv_msgbox_add_text(box, filename(path));
    lv_msgbox_add_text(box, path);
    if (page->selected.directory) lv_msgbox_add_text(box, "文件夹");
    else lv_msgbox_add_text_fmt(box, "%llu 字节", (unsigned long long)page->selected.size);
    const time_t modified = (time_t)page->selected.modified_s;
    struct tm date;
    char when[24];
    if (modified && localtime_r(&modified, &date) && strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &date))
        lv_msgbox_add_text(box, when);
    lv_obj_t *close = files_dialog_button(box, "关闭", false, files_dialog_cancel, page);
    app_ui_modal_focus(box, close);
}

enum { FILE_OPTION_DETAILS, FILE_OPTION_COPY, FILE_OPTION_MOVE, FILE_OPTION_DELETE };

static void files_item_option(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    files_page_t *page = ui->page_context;
    if (!page || !page->dialog) return;
    const uintptr_t action = (uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    lv_msgbox_close(page->dialog);
    if (action == FILE_OPTION_DETAILS) files_show_details(ui, page);
    else if (!page->selected.directory) {
        if (action == FILE_OPTION_DELETE) files_confirm_delete(ui, page);
        else {
            strlcpy(page->clipboard, page->selected.path, sizeof(page->clipboard));
            page->clipboard_generation = page->media_generation;
            page->cut = action == FILE_OPTION_MOVE;
            app_ui_label_text(page->info, "选择目标文件夹，再点粘贴");
            files_controls(page);
        }
    }
}

static void files_item_options(app_ui_t *ui, files_page_t *page, const app_io_entry_t *entry)
{
    lv_obj_t *box = files_dialog(ui, page, "项目操作");
    if (!box) return;
    page->selected = *entry;
    page->select_mode = false;
    files_controls(page);
    char name[sizeof(entry->path)];
    strlcpy(name, filename(entry->path), sizeof(name));
    app_text_sanitize_utf8(name, strlen(name));
    lv_msgbox_add_text(box, name);
    lv_obj_t *actions = lv_obj_create(lv_msgbox_get_content(box));
    lv_obj_remove_style_all(actions);
    lv_obj_remove_flag(actions, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(actions, LV_PCT(100), entry->directory ? 44 : 98);
    static const char *const names[] = {"详情", "复制", "移动", "删除"};
    const unsigned count = entry->directory ? 1 : sizeof(names) / sizeof(names[0]);
    for (unsigned i = 0; i < count; ++i) {
        lv_obj_t *action = lv_button_create(actions);
        lv_obj_set_size(action, 154, 44);
        lv_obj_set_pos(action, 166 * (i % 2), 54 * (i / 2));
        app_catalog_control(action, i == FILE_OPTION_DELETE ? 0x573146 : 0x20344C);
        lv_obj_set_user_data(action, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(action, files_item_option, LV_EVENT_CLICKED, ui);
        app_ui_add_text(action, names[i], LV_ALIGN_CENTER, 0, 0, 0xEAF2FF, NULL);
    }
    lv_obj_t *cancel = files_dialog_button(box, "取消", false, files_dialog_cancel, page);
    app_ui_modal_focus(box, cancel);
}

static void file_clicked(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    if (!ui || ui->page_cleanup != files_cleanup) return;
    files_page_t *page = ui->page_context;
    const size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (!page || index >= page->count || page->loading || page->mutating || page->dialog || ui->io_complete) return;
    const bool held = lv_event_get_code(event) == LV_EVENT_LONG_PRESSED;
    if (!files_storage_ready(ui) || aether_media_fs_generation(ui->storage, page->directory) != page->media_generation) {
        files_refresh(ui); return;
    }
    app_io_entry_t item = page->entries[index];
    if (held || page->select_mode) {
        files_item_options(ui, page, &item);
        return;
    }
    if (item.directory) {
        files_capture_entry_route(ui, page);
        page->feedback = NULL;
        page->restore_scroll_y = 0;
        strlcpy(page->directory, item.path, sizeof(page->directory));
        files_update_back(ui, page);
        files_clear_rows(page);
        files_refresh(ui);
        return;
    }
    const char *ext = strrchr(item.path, '.');
    if (ext && !strcasecmp(ext, ".txt")) app_open_reader_path(ui, item.path);
    else if (ext && (!strcasecmp(ext, ".wav") || !strcasecmp(ext, ".mp3") || !strcasecmp(ext, ".m4a"))) {
        files_capture_entry_route(ui, page);
        app_files_return_t previous = {
            .exit_path_length = page->exit_path_length,
            .scroll_y = lv_obj_get_scroll_y(page->rows.viewport),
            .media_generation = page->media_generation,
            .parent = page->exit_back,
        };
        /* Opening the player frees this browser page. Keep only its return location. */
        strlcpy(previous.directory, page->directory, sizeof(previous.directory));
        app_open_music_path(ui, item.path);
        ui->files_return = previous;
        ui->page_back = files_back_from_audio;
    }
    else if (ext && (!strcasecmp(ext, ".mp4") || !strcasecmp(ext, ".m4v") || !strcasecmp(ext, ".avi") || !strcasecmp(ext, ".mjpg") || !strcasecmp(ext, ".mjpeg"))) app_open_video_path(ui, item.path);
    else if (ext && !strcasecmp(ext, ".nes")) app_open_nes_path(ui, item.path);
    else app_ui_label_fmt(page->info, "文件大小 %llu 字节；此格式暂不支持打开", (unsigned long long)item.size);
}

static lv_obj_t *create_file_row(lv_obj_t *parent, void *context)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, 396, 66);
    lv_obj_set_style_pad_all(row, 9, 0);
    app_catalog_row(row);
    lv_obj_t *image = lv_image_create(row);
    lv_obj_align(image, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(image, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(image, lv_color_hex(0x8DE0E1), 0);
    lv_obj_t *title = app_ui_add_text(row, "", LV_ALIGN_TOP_LEFT, 60, 0,
                                     0xEAF2FF, app_ui_chinese_font());
    lv_obj_set_size(title, 310, 21);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t *metadata = app_ui_add_text(row, "", LV_ALIGN_TOP_LEFT, 60, 26,
                                        0x91A8C7, app_ui_chinese_font());
    lv_obj_set_size(metadata, 310, 20);
    lv_label_set_long_mode(metadata, LV_LABEL_LONG_DOT);
    lv_obj_add_event_cb(row, file_clicked, LV_EVENT_CLICKED, context);
    lv_obj_add_event_cb(row, file_clicked, LV_EVENT_LONG_PRESSED, context);
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
    if (page->query[0]) {
        const size_t root = strlen(page->directory);
        const char *slash = strrchr(entry->path, '/');
        char folder[sizeof(entry->path)];
        if (slash && slash > entry->path + root) {
            const size_t length = (size_t)(slash - entry->path) - root - 1U;
            memcpy(folder, entry->path + root + 1U, length);
            folder[length] = '\0';
            app_text_sanitize_utf8(folder, length);
            app_ui_label_text(metadata, folder);
        } else app_ui_label_text(metadata, "当前文件夹");
    } else if (entry->directory) app_ui_label_text(metadata, "文件夹");
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
    const bool search = result->operation == APP_IO_SEARCH_FILES;
    if (aether_media_fs_generation(ui->storage, page->directory) != page->media_generation) {
        files_refresh(ui); return;
    }
    page->mounted = files_storage_ready(ui);
    if (!page->mounted) {
        files_clear_rows(page);
        files_unavailable(ui,page);
        files_controls(page);
        return;
    }
    if (result->error != ESP_OK) {
        if (!search) files_clear_rows(page);
        page->restore_scroll_y = 0;
        app_ui_label_fmt(page->info, "%s：%s", search ? "查找失败" : "读取失败", esp_err_to_name(result->error));
        files_controls(page);
        return;
    }
    app_io_entry_t *entries = result->count ? heap_caps_malloc(result->count * sizeof(*entries),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (result->count && !entries) {
        if (!search) files_clear_rows(page);
        page->restore_scroll_y = 0;
        app_ui_label_text(page->info, "内存不足，无法显示目录");
        files_controls(page);
        return;
    }
    if (result->count) memcpy(entries, result->entries, result->count * sizeof(*entries));
    free(page->entries);
    page->entries = entries;
    if (search) strlcpy(page->query, page->requested_query, sizeof(page->query));
    else page->query[0] = '\0';
    page->count = result->count;
    app_scroll_list_set_count(&page->rows, page->count);
    if (page->restore_scroll_y > 0 && page->restore_generation == page->media_generation) {
        lv_obj_update_layout(page->rows.viewport);
        lv_obj_scroll_to_y(page->rows.viewport, page->restore_scroll_y, LV_ANIM_OFF);
    }
    page->restore_scroll_y = 0;
    app_ui_label_fmt(page->info, "%s%u 项%s", search ? "查找 · " : "", (unsigned)result->total_count,
        result->truncated ? " · 部分项目未索引" : "");
    if (!page->count) app_ui_label_text(page->info, search ? "没有匹配的文件" : "这个文件夹为空");
    if (page->feedback) app_ui_label_text(page->info, page->feedback);
    files_controls(page);
}

static void files_refresh(app_ui_t *ui)
{
    files_page_t *page = ui->page_context;
    page->select_mode = false;
    page->query[0] = '\0';
    char path[sizeof(page->directory)];
    strlcpy(path, page->directory, sizeof(path));
    app_text_sanitize_utf8(path, strlen(path));
    app_ui_label_text(page->path, path);
    page->media_generation = aether_media_fs_generation(ui->storage, page->directory);
    if (page->restore_generation != page->media_generation) page->restore_scroll_y = 0;
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

static void files_search_start(app_ui_t *ui, const char *query)
{
    files_page_t *page = ui->page_context;
    if (!files_storage_ready(ui) || aether_media_fs_generation(ui->storage, page->directory) != page->media_generation) {
        files_refresh(ui);
        return;
    }
    app_io_request_t request = {.operation = APP_IO_SEARCH_FILES, .limit = FILES_LIMIT,
        .media_generation = page->media_generation};
    strlcpy(page->requested_query, query, sizeof(page->requested_query));
    strlcpy(request.query, page->requested_query, sizeof(request.query));
    strlcpy(request.path, page->directory, sizeof(request.path));
    page->select_mode = false;
    page->loading = true;
    app_ui_label_text(page->info, "正在查找…");
    files_controls(page);
    app_ui_request_io_ex(ui, &request, files_loaded);
}

static void files_search_submit(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    files_page_t *page = ui->page_context;
    if (!page || !page->dialog || !page->search_field || ui->io_complete) return;
    const char *text = lv_textarea_get_text(page->search_field);
    while (*text == ' ') ++text;
    size_t length = strlen(text);
    while (length && text[length - 1] == ' ') --length;
    char query[sizeof(page->query)];
    if (!length || length >= sizeof(query)) {
        app_ui_label_text(page->search_notice, length ? "名称太长，请缩短后重试" : "请输入文件名的一部分");
        return;
    }
    memcpy(query, text, length);
    query[length] = '\0';
    page->feedback = NULL;
    page->restore_scroll_y = 0;
    lv_msgbox_close_async(page->dialog);
    files_search_start(ui, query);
}

static void files_search_layout(void *context, bool visible)
{
    files_page_t *page = context;
    if (page->dialog) lv_obj_align(page->dialog, LV_ALIGN_CENTER, 0, visible ? -114 : 0);
}

static void files_search_dialog(app_ui_t *ui, files_page_t *page)
{
    lv_obj_t *box = files_dialog(ui, page, "查找文件");
    if (!box) return;
    page->search_field = lv_textarea_create(lv_msgbox_get_content(box));
    lv_obj_set_size(page->search_field, LV_PCT(100), 44);
    lv_obj_set_style_text_font(page->search_field, app_ui_chinese_font(), 0);
    lv_obj_set_style_text_color(page->search_field, lv_color_hex(0xEAF2FF), 0);
    app_catalog_control(page->search_field, 0x20344C);
    app_text_input_style_cursor(page->search_field, 0x92E7F1);
    lv_textarea_set_one_line(page->search_field, true);
    lv_textarea_set_max_length(page->search_field, sizeof(page->query) - 1);
    lv_textarea_set_placeholder_text(page->search_field, "文件名");
    lv_textarea_set_text(page->search_field, page->query);
    page->search_notice = lv_msgbox_add_text(box, "");
    lv_obj_set_style_text_color(page->search_notice, lv_color_hex(0xFFC28A), 0);
    lv_obj_add_event_cb(page->search_field, files_search_submit, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(page->search_field, files_dialog_cancel, LV_EVENT_CANCEL, page);
    files_dialog_button(box, "取消", false, files_dialog_cancel, page);
    files_dialog_button(box, "搜索", false, files_search_submit, ui);
    lv_obj_t *dock = app_text_input_create(lv_obj_get_parent(box), page->search_field, true, files_search_layout, page);
    if (dock) app_text_input_show(dock, true);
    else app_ui_label_text(page->search_notice, "屏幕键盘不可用");
    app_ui_modal_focus(box, page->search_field);
}

enum { FILES_REFRESH, FILES_QUICK, FILES_SEARCH };

static void files_action(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    files_page_t *page = ui->page_context;
    if (!page || ui->io_complete || page->dialog || page->mutating) return;
    intptr_t action = (intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    page->feedback = NULL;
    if (action == FILES_SEARCH) {
        if (files_storage_ready(ui)) files_search_dialog(ui, page);
        return;
    }
    if (action == FILES_QUICK) {
        if (!files_storage_ready(ui)) return;
        if (aether_media_fs_generation(ui->storage, page->directory) != page->media_generation) {
            files_refresh(ui);
            return;
        }
        if (page->clipboard[0]) { files_paste(ui, page); return; }
        page->select_mode = !page->select_mode;
        if (page->select_mode) app_ui_label_text(page->info, "选择要操作的项目");
        else app_ui_label_fmt(page->info, "%u 项", (unsigned)page->count);
        files_controls(page);
        return;
    }
    if (action == FILES_REFRESH && !page->mounted && page->retry_allowed && strncmp(page->directory,"/usb",4)) {
        const esp_err_t error = aether_storage_request_retry(ui->storage);
        if (error != ESP_OK) { app_ui_label_fmt(page->info,"TF 重试失败：%s",esp_err_to_name(error));return; }
        page->retry_after = lv_tick_get()+1000;
    }
    files_refresh(ui);
}
static lv_obj_t *files_button(app_ui_t *ui, const char *text, int x, int y, intptr_t action)
{
    lv_obj_t *button = lv_button_create(ui->content);
    lv_obj_set_pos(button, x, y); lv_obj_set_size(button, 54, 44);
    app_catalog_control(button, 0x20344C);
    lv_obj_set_user_data(button, (void *)action);
    lv_obj_add_event_cb(button, files_action, LV_EVENT_CLICKED, ui);
    app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0, 0x9DDCEB,
                    action == FILES_SEARCH ? NULL : &lv_font_montserrat_24);
    return button;
}
static void files_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    files_page_t *page = ui->page_context;
    if (!page) return;
    if (page->loading || page->mutating) return;
    const bool ready = files_storage_ready(ui);
    const uint32_t generation = aether_media_fs_generation(ui->storage, page->directory);
    if (ready != page->mounted || generation != page->media_generation) {
        if (page->dialog) lv_msgbox_close(page->dialog);
        page->select_mode = false;
        page->query[0] = '\0';
        page->feedback = NULL;
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
    if (!page || ui->io_complete || page->dialog || page->mutating) return;
    files_capture_entry_route(ui, page);
    page->feedback = NULL;
    page->restore_scroll_y = 0;
    strlcpy(page->directory, lv_dropdown_get_selected(page->source) ? "/usb" : AETHER_STORAGE_MOUNT_POINT,
            sizeof(page->directory));
    page->exit_path_length = strlen(page->directory);
    files_update_back(ui, page);
    files_clear_rows(page);
    files_refresh(ui);
}
static void files_open(app_ui_t *ui, const char *path, const app_files_return_t *previous)
{
    char selected[192];
    const char *source = path && strlen(path) < sizeof(selected) && aether_media_fs_safe_path(path) ?
        path : aether_media_fs_default_root(ui->storage);
    strlcpy(selected, source, sizeof(selected));
    lv_obj_t *content = app_ui_begin_page(ui, "文件浏览");
    if (previous) ui->page_back = previous->parent;
    files_page_t *page = calloc(1, sizeof(*page));
    if (!page) { app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF8888, NULL); return; }
    ui->page_context = page; ui->page_cleanup = files_cleanup;
    page->exit_back = ui->page_back;
    strlcpy(page->directory, selected, sizeof(page->directory));
    const char *root = !strncmp(selected, "/usb", 4) ? "/usb" : AETHER_STORAGE_MOUNT_POINT;
    page->exit_path_length = strlen(root);
    if (previous && !strcmp(selected, previous->directory)) {
        page->exit_path_length = previous->exit_path_length;
        page->restore_generation = previous->media_generation;
        page->restore_scroll_y = previous->scroll_y;
    }
    files_update_back(ui, page);
    page->mounted = files_storage_ready(ui);
    app_catalog_panel(content);
    /* Keep the 408px content area when adding the panel border. */
    lv_obj_set_style_pad_all(content, 19, 0);
    page->path = app_ui_add_text(content, page->directory, LV_ALIGN_TOP_LEFT, 3, 55, 0x98D5E4, NULL);
    lv_obj_set_width(page->path, 402); lv_label_set_long_mode(page->path, LV_LABEL_LONG_DOT);
    page->source = lv_dropdown_create(content);
    lv_obj_set_pos(page->source, 0, 0); lv_obj_set_size(page->source, 211, 44);
    lv_obj_set_style_text_font(page->source, app_ui_chinese_font(), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(page->source), app_ui_chinese_font(), 0);
    app_catalog_control(page->source, 0x20344C);
    lv_obj_set_style_text_color(page->source, lv_color_hex(0xEAF2FF), 0);
    lv_obj_set_style_pad_top(page->source, 11, 0);
    lv_obj_set_style_bg_color(lv_dropdown_get_list(page->source), lv_color_hex(0x18243A), 0);
    lv_obj_set_style_text_color(lv_dropdown_get_list(page->source), lv_color_hex(0xEAF2FF), 0);
    lv_dropdown_set_options(page->source, "TF 卡\nUSB U盘");
    lv_dropdown_set_selected(page->source, !strncmp(page->directory, "/usb", 4));
    lv_obj_add_event_cb(page->source, files_source, LV_EVENT_VALUE_CHANGED, ui);
    page->search = files_button(ui, "查找", 225, 0, FILES_SEARCH);
    page->quick = files_button(ui, LV_SYMBOL_LIST, 289, 0, FILES_QUICK);
    lv_obj_set_style_bg_color(page->quick, lv_color_hex(0x315A72), LV_STATE_CHECKED);
    page->refresh = files_button(ui, LV_SYMBOL_REFRESH, 354, 0, FILES_REFRESH);
    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_pos(list, 0, 81);
    lv_obj_set_size(list, 408, 225);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    app_scroll_list_init(&page->rows, list, 72, create_file_row, bind_file_row, ui);
    page->info = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 3, 312, 0xAFC6E9, NULL);
    lv_obj_set_width(page->info, 405);
    lv_label_set_long_mode(page->info, LV_LABEL_LONG_WRAP);
    app_ui_set_timer(ui, files_timer, 500); files_refresh(ui);
}

void app_open_files_path(app_ui_t *ui, const char *path)
{
    files_open(ui, path, NULL);
}

void app_open_files(app_ui_t *ui)
{
    app_open_files_path(ui, NULL);
}
