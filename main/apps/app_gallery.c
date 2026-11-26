#include "app_ui_internal.h"
#include "app_catalog_ui.h"
#include "app_gallery_format.h"
#include "app_scroll_list.h"
#include "app_text.h"
#include "app_media_image.h"
#include "app_theme.h"
#include "app_wallpaper_store.h"
#include "aether_media_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#define GALLERY_CATALOG_LIMIT 4096U
#define GALLERY_IMAGE_WIDTH 408U
#define GALLERY_IMAGE_HEIGHT 276U

static const char *const gallery_roots[] = {AETHER_STORAGE_MOUNT_POINT, AETHER_USB_STORAGE_MOUNT_POINT};
static const char *const gallery_root_names[] = {"TF 卡", "USB"};

typedef struct {
    uint32_t generations[2];
    unsigned sources;
    bool exported, blocked;
} gallery_media_t;

typedef struct {
    atomic_uint users;
    atomic_bool cancelled;
    atomic_bool done;
    aether_storage_handle_t storage;
    uint32_t generation;
    char path[192];
    esp_err_t error;
    app_media_image_t image;
    bool wallpaper;
} gallery_job_t;

typedef struct {
    app_io_entry_t *entries;
    size_t count, selected;
    gallery_media_t media;
    bool loading, wallpaper, saving;
    app_scroll_list_t rows;
    lv_obj_t *source_label, *summary, *refresh;
    lv_obj_t *viewer, *image, *image_label, *older, *newer, *apply;
    void (*list_back)(app_ui_t *);
    void (*pending_exit)(app_ui_t *);
    lv_image_dsc_t descriptor;
    uint16_t *pixels;
    gallery_job_t *job;
} gallery_page_t;

static atomic_bool decode_running;

static void gallery_refresh(app_ui_t *ui);
static void gallery_show_image(app_ui_t *ui, size_t index);

static const char *file_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void set_enabled(lv_obj_t *object, bool enables)
{
    if (enables) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}

static gallery_media_t gallery_media(app_ui_t *ui, bool wallpaper)
{
    gallery_media_t media = {.exported = app_io_usb_storage_blocked(ui->io),
                             .blocked = app_ui_input_blocked(ui)};
    if (media.exported || media.blocked) return media;
    const unsigned root_count = wallpaper ? 1U : sizeof(gallery_roots) / sizeof(gallery_roots[0]);
    for (unsigned i = 0; i < root_count; ++i) {
        if (aether_media_fs_path_ready(ui->storage, gallery_roots[i])) {
            media.sources |= 1U << i;
            media.generations[i] = aether_media_fs_generation(ui->storage, gallery_roots[i]);
        }
    }
    return media;
}

static bool gallery_media_matches(const gallery_media_t *a, const gallery_media_t *b)
{
    return a->sources == b->sources && a->exported == b->exported && a->blocked == b->blocked &&
           a->generations[0] == b->generations[0] && a->generations[1] == b->generations[1];
}

static unsigned gallery_source(const char *path)
{
    const size_t usb = strlen(AETHER_USB_STORAGE_MOUNT_POINT);
    return !strncmp(path, AETHER_USB_STORAGE_MOUNT_POINT, usb) && path[usb] == '/';
}

static bool gallery_selection_ready(app_ui_t *ui, const gallery_page_t *page, const char *path)
{
    const unsigned source = gallery_source(path);
    return !app_io_usb_storage_blocked(ui->io) && !app_ui_input_blocked(ui) &&
           (page->media.sources & (1U << source)) && aether_media_fs_path_ready(ui->storage, path) &&
           aether_media_fs_generation(ui->storage, path) == page->media.generations[source];
}

static void job_release(gallery_job_t *job)
{
    if (atomic_fetch_sub(&job->users, 1) == 1) {
        free(job->image.pixels);
        free(job);
    }
}

static void image_decode_task(void *arg)
{
    gallery_job_t *job = arg;
    job->error = app_media_image_read(job->storage, job->path, job->generation, &job->cancelled,
                                     job->wallpaper ? APP_WALLPAPER_SIZE : GALLERY_IMAGE_WIDTH,
                                     job->wallpaper ? APP_WALLPAPER_SIZE : GALLERY_IMAGE_HEIGHT,
                                     job->wallpaper, &job->image);
    atomic_store(&decode_running, false);
    atomic_store_explicit(&job->done, true, memory_order_release);
    job_release(job);
    vTaskDelete(NULL);
}

static void gallery_drop_pixels(gallery_page_t *page)
{
    if (page->image && page->pixels) {
        lv_image_set_src(page->image, NULL);
        lv_image_cache_drop(&page->descriptor);
    }
    free(page->pixels);
    page->pixels = NULL;
}

static void gallery_cancel_decode(gallery_page_t *page)
{
    if (page->job) {
        atomic_store(&page->job->cancelled, true);
        job_release(page->job);
        page->job = NULL;
    }
}

static void gallery_close_viewer(app_ui_t *ui, gallery_page_t *page)
{
    gallery_cancel_decode(page);
    gallery_drop_pixels(page);
    if (page->viewer) {
        lv_obj_delete(page->viewer);
        page->viewer = NULL;
        page->image = NULL;
        page->apply = NULL;
        if (page->rows.viewport) {
            ui->page_back = page->list_back;
            lv_obj_remove_flag(page->rows.viewport, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void gallery_viewer_back(app_ui_t *ui)
{
    gallery_page_t *page = ui->page_context;
    if (page) gallery_close_viewer(ui, page);
}

static void gallery_cleanup(app_ui_t *ui)
{
    gallery_page_t *page = ui->page_context;
    if (page) {
        gallery_close_viewer(ui, page);
        app_scroll_list_deinit(&page->rows);
        free(page->entries);
        free(page);
    }
    ui->page_context = NULL;
}

static lv_obj_t *gallery_button(lv_obj_t *parent, const char *text, int x, int y, int width,
                               lv_event_cb_t handler, app_ui_t *ui, intptr_t action)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 44);
    app_catalog_control(button, 0x20344C);
    lv_obj_set_user_data(button, (void *)action);
    lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED, ui);
    app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0, 0x9DDCEB, &lv_font_montserrat_24);
    return button;
}

static size_t adjacent_photo(const gallery_page_t *page, int direction)
{
    const int index = (int)page->selected + direction;
    return index >= 0 && (size_t)index < page->count ? (size_t)index : SIZE_MAX;
}

static void viewer_controls(gallery_page_t *page)
{
    if (!page->rows.viewport) return;
    const bool ready = !page->job && !page->saving;
    set_enabled(page->older, ready && adjacent_photo(page, -1) != SIZE_MAX);
    set_enabled(page->newer, ready && adjacent_photo(page, 1) != SIZE_MAX);
    if (page->apply) set_enabled(page->apply, ready && page->pixels && app_theme_wallpaper_available());
}

static void viewer_action(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    gallery_page_t *page = ui->page_context;
    const intptr_t action = (intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (!page || !page->viewer || page->saving) return;
    const size_t index = adjacent_photo(page, (int)action);
    if (!page->job && index != SIZE_MAX) gallery_show_image(ui, index);
}

static bool wallpaper_can_exit(app_ui_t *ui, void (*destination)(app_ui_t *))
{
    gallery_page_t *page = ui->page_context;
    if (!page->saving) return true;
    page->pending_exit = destination;
    return false;
}

static void apply_wallpaper(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    gallery_page_t *page = ui->page_context;
    if (!page || !page->wallpaper || page->saving || !page->pixels ||
        !gallery_selection_ready(ui, page, page->entries[page->selected].path)) return;
    const esp_err_t error = app_theme_write_wallpaper(page->pixels, page->descriptor.header.w,
                                                      page->descriptor.header.h);
    if (error != ESP_OK) {
        app_ui_label_text(page->image_label, error == ESP_ERR_NO_MEM ? "内存不足，无法设置壁纸" : "无法保存壁纸，请重试");
        return;
    }
    /* The writer now owns the decoded buffer. Detach the image before the
     * UI task polls the writer and transfers or disposes of it. */
    lv_image_set_src(page->image, NULL);
    lv_image_cache_drop(&page->descriptor);
    page->pixels = NULL;
    page->saving = true;
    app_ui_label_text(page->image_label, "正在保存壁纸…");
    viewer_controls(page);
    lv_timer_set_period(ui->timer, 40);
}

static void gallery_show_image(app_ui_t *ui, size_t index)
{
    gallery_page_t *page = ui->page_context;
    if (!page || page->job || index >= page->count) return;
    const char *path = page->entries[index].path;
    if (!gallery_selection_ready(ui, page, path)) return;
    if (!page->viewer) {
        page->viewer = lv_obj_create(ui->content);
        lv_obj_set_size(page->viewer, 408, 354);
        lv_obj_set_pos(page->viewer, 0, 0);
        lv_obj_remove_flag(page->viewer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(page->viewer, lv_color_hex(0x03060B), 0);
        lv_obj_set_style_pad_all(page->viewer, 0, 0);
        lv_obj_set_style_border_width(page->viewer, 0, 0);
        lv_obj_set_style_radius(page->viewer, 14, 0);
        page->image = lv_image_create(page->viewer);
        page->image_label = app_ui_add_text(page->viewer, "", LV_ALIGN_TOP_LEFT, 8, 280, 0xB7CCE8, NULL);
        lv_obj_set_width(page->image_label, 392);
        lv_label_set_long_mode(page->image_label, LV_LABEL_LONG_DOT);
        if (page->rows.viewport) {
            page->older = gallery_button(page->viewer, LV_SYMBOL_PREV, 240, 307, 72, viewer_action, ui, -1);
            page->newer = gallery_button(page->viewer, LV_SYMBOL_NEXT, 328, 307, 72, viewer_action, ui, 1);
            page->list_back = ui->page_back;
            ui->page_back = gallery_viewer_back;
            lv_obj_stop_scroll_anim(page->rows.viewport);
            lv_obj_add_flag(page->rows.viewport, LV_OBJ_FLAG_HIDDEN);
        }
        if (page->wallpaper) {
            page->apply = gallery_button(page->viewer, "设为壁纸", 8, 307, 146, apply_wallpaper, ui, 0);
            lv_obj_set_style_text_font(lv_obj_get_child(page->apply, 0), app_ui_chinese_font(), 0);
        }
    }
    page->selected = index;
    gallery_drop_pixels(page);
    if (atomic_exchange(&decode_running, true)) {
        app_ui_label_text(page->image_label, "上一张正在结束，请返回后重试");
        viewer_controls(page);
        return;
    }
    gallery_job_t *job = calloc(1, sizeof(*job));
    if (job) {
        atomic_init(&job->users, 2);
        atomic_init(&job->cancelled, false);
        atomic_init(&job->done, false);
        job->storage = ui->storage;
        job->wallpaper = page->wallpaper;
        job->generation = page->media.generations[gallery_source(path)];
        strlcpy(job->path, path, sizeof(job->path));
        /* PNG's Huffman tables are larger than the streaming BMP/GIF stack. */
        const unsigned stack = app_gallery_format(path) == APP_IMAGE_PNG ? 12288 : 4096;
        if (xTaskCreate(image_decode_task, "gallery", stack, job, 3, NULL) != pdPASS) {
            free(job);
            job = NULL;
        }
    }
    page->job = job;
    if (!job) atomic_store(&decode_running, false);
    app_ui_label_text(page->image_label, job ? "正在读图…" : "内存不足，无法打开图片");
    viewer_controls(page);
    if (job) {
        lv_timer_set_period(ui->timer, 40);
        lv_timer_ready(ui->timer);
    }
}

static void gallery_present(gallery_page_t *page)
{
    gallery_job_t *job = page->job;
    if (!job || !atomic_load_explicit(&job->done, memory_order_acquire)) return;
    page->job = NULL;
    if (job->error == ESP_OK) {
        const uint16_t width = job->image.width, height = job->image.height;
        page->pixels = job->image.pixels;
        job->image.pixels = NULL;
        page->descriptor = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                       .w = width, .h = height, .stride = width * 2U},
            .data_size = (uint32_t)width * height * 2U,
            .data = (const uint8_t *)page->pixels,
        };
        lv_image_set_src(page->image, &page->descriptor);
        lv_obj_set_pos(page->image, ((int)GALLERY_IMAGE_WIDTH - width) / 2,
                        ((int)GALLERY_IMAGE_HEIGHT - height) / 2);
        lv_image_set_pivot(page->image, width / 2, height / 2);
        const uint32_t scale = LV_MIN(GALLERY_IMAGE_WIDTH * LV_SCALE_NONE / width,
                                      GALLERY_IMAGE_HEIGHT * LV_SCALE_NONE / height);
        lv_image_set_scale(page->image, LV_MIN(scale, LV_SCALE_NONE));
        char name[192];
        strlcpy(name, file_name(job->path), sizeof(name));
        app_text_sanitize_utf8(name, strlen(name));
        if (page->rows.viewport) {
            app_ui_label_fmt(page->image_label, "%u / %u  ·  %s", (unsigned)page->selected + 1U,
                             (unsigned)page->count, name);
        } else app_ui_label_text(page->image_label, name);
    } else {
        const char *reason = job->error == ESP_ERR_NOT_SUPPORTED ? "图片编码暂不支持" :
                             job->error == ESP_ERR_NO_MEM ? "内存不足" :
                             job->error == ESP_ERR_INVALID_SIZE ? "图片太大或尺寸无效" :
                             job->error == ESP_ERR_NOT_FOUND ? "文件已移除" : "数据损坏或解码失败";
        app_ui_label_fmt(page->image_label, "无法打开图片 · %s", reason);
    }
    job_release(job);
    viewer_controls(page);
}

static void catalog_controls(gallery_page_t *page)
{
    set_enabled(page->refresh, page->media.sources && !page->loading);
}

static void gallery_entry_clicked(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    gallery_page_t *page = ui->page_context;
    const size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (page && !page->loading && !page->viewer) gallery_show_image(ui, index);
}

static lv_obj_t *create_gallery_row(lv_obj_t *parent, void *context)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, LV_PCT(100), 52);
    app_catalog_row(button);
    lv_obj_set_style_pad_all(button, 8, 0);
    app_catalog_badge(app_ui_add_text(button, LV_SYMBOL_IMAGE, LV_ALIGN_LEFT_MID, 0, 0,
                                      0x8DE0E1, &lv_font_montserrat_16));
    lv_obj_t *name = app_ui_add_text(button, "", LV_ALIGN_LEFT_MID, 46, -10, 0xEAF2FF, NULL);
    lv_obj_t *location = app_ui_add_text(button, "", LV_ALIGN_LEFT_MID, 46, 10, 0x9CB2CF, NULL);
    lv_obj_set_width(name, 340);
    lv_obj_set_width(location, 340);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(location, LV_LABEL_LONG_DOT);
    lv_obj_add_event_cb(button, gallery_entry_clicked, LV_EVENT_CLICKED, context);
    return button;
}

static void bind_gallery_row(lv_obj_t *row, size_t index, void *context)
{
    app_ui_t *ui = context;
    const gallery_page_t *page = ui->page_context;
    const char *path = page->entries[index].path;
    const char *base = file_name(path);
    char name[192];
    strlcpy(name, base, sizeof(name));
    app_text_sanitize_utf8(name, strlen(name));
    app_ui_label_text(lv_obj_get_child(row, 1), name);
    const unsigned source = gallery_source(path);
    const char *folder = path + strlen(gallery_roots[source]) + 1;
    const int folder_length = base > folder ? (int)(base - folder - 1) : 0;
    char location[220];
    if (folder_length) snprintf(location, sizeof(location), "%s · %.*s", gallery_root_names[source], folder_length, folder);
    else strlcpy(location, gallery_root_names[source], sizeof(location));
    app_text_sanitize_utf8(location, strlen(location));
    app_ui_label_text(lv_obj_get_child(row, 2), location);
}

static void gallery_catalog_ready(app_ui_t *ui, const app_io_result_t *result)
{
    gallery_page_t *page = ui->page_context;
    page->loading = false;
    const gallery_media_t current = gallery_media(ui, page->wallpaper);
    if (!gallery_media_matches(&page->media, &current)) {
        gallery_refresh(ui);
        return;
    }
    if (result->error != ESP_OK) {
        const char *error = result->error == ESP_ERR_NO_MEM ? "内存不足，无法加载图片列表" :
            result->error == ESP_ERR_NOT_FOUND ? "未找到可读取的存储，点击刷新" : "扫描图片失败，点击刷新重试";
        app_ui_label_text(page->summary, error);
        catalog_controls(page);
        return;
    }
    if (result->count) {
        page->entries = heap_caps_malloc(result->count * sizeof(*page->entries), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!page->entries) {
            app_ui_label_text(page->summary, "内存不足，无法显示图片列表");
            catalog_controls(page);
            return;
        }
        memcpy(page->entries, result->entries, result->count * sizeof(*page->entries));
    }
    page->count = result->count;
    app_scroll_list_set_count(&page->rows, page->count);
    if (result->total_count > page->count) {
        app_ui_label_fmt(page->summary, "显示最近 %u / %u 张", (unsigned)page->count,
                         (unsigned)result->total_count);
    } else if (result->truncated) {
        app_ui_label_fmt(page->summary, "显示 %u 张 · 部分目录未读取", (unsigned)page->count);
    } else if (page->count) {
        app_ui_label_fmt(page->summary, "共 %u 张", (unsigned)page->count);
    } else {
        app_ui_label_text(page->summary, "没有可显示的图片 · JPEG、PNG、BMP、GIF");
    }
    catalog_controls(page);
}

static void gallery_refresh(app_ui_t *ui)
{
    gallery_page_t *page = ui->page_context;
    if (!page) return;
    if (page->loading) {
        app_io_cancel(ui->io, ui->page_generation);
        ui->io_complete = NULL;
        page->loading = false;
    }
    if (ui->io_complete) return;
    gallery_close_viewer(ui, page);
    app_scroll_list_set_count(&page->rows, 0);
    free(page->entries);
    page->entries = NULL;
    page->count = 0;
    page->media = gallery_media(ui, page->wallpaper);
    const char *sources = page->media.sources == 3U ? "TF 卡 · USB" :
                          page->media.sources ? gallery_root_names[page->media.sources == 2U] : "";
    app_ui_label_text(page->source_label, sources);
    if (!page->media.sources) {
        const char *status = page->media.exported ? "请先退出 USB 文件传输" :
            page->media.blocked ? "正在等待存储释放…" :
            page->wallpaper ? "请插入 TF 卡" : "请插入 TF 卡或 USB 存储";
        app_ui_label_text(page->summary, status);
        catalog_controls(page);
        return;
    }
    page->loading = true;
    app_ui_label_text(page->summary, "正在查找图片…");
    catalog_controls(page);
    app_io_request_t request = {
        .operation = APP_IO_CATALOG, .filter = APP_IO_IMAGES,
        .limit = GALLERY_CATALOG_LIMIT,
    };
    if (page->wallpaper) strlcpy(request.path, AETHER_STORAGE_MOUNT_POINT, sizeof(request.path));
    app_ui_request_io_ex(ui, &request, gallery_catalog_ready);
}

static void gallery_action(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    gallery_page_t *page = ui->page_context;
    if (!page || page->loading || page->viewer) return;
    gallery_refresh(ui);
}

static void gallery_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    gallery_page_t *page = ui->page_context;
    if (!page) return;
    if (!page->rows.viewport) {
        if (page->viewer && !gallery_selection_ready(ui, page, page->entries[0].path)) {
            gallery_cancel_decode(page);
            gallery_drop_pixels(page);
            app_ui_label_text(page->image_label, "图片来源已断开");
            lv_timer_pause(timer);
        } else gallery_present(page);
        if (!page->job) lv_timer_set_period(timer, 250);
        return;
    }
    if (page->saving) {
        const esp_err_t result = app_theme_write_status();
        if (result == ESP_ERR_NOT_FINISHED) return;
        page->saving = false;
        if (page->pending_exit) { page->pending_exit(ui); return; }
        if (result == ESP_OK) { app_open_theme(ui); return; }
        app_ui_label_text(page->image_label, "壁纸保存失败，返回列表后重试");
        viewer_controls(page);
    }
    const gallery_media_t current = gallery_media(ui, page->wallpaper);
    if (!gallery_media_matches(&page->media, &current)) {
        gallery_refresh(ui);
    } else if (page->viewer) {
        gallery_present(page);
    }
    lv_timer_set_period(timer, page->job ? 40 : 250);
}

static void open_gallery(app_ui_t *ui, bool wallpaper)
{
    lv_obj_t *content = app_ui_begin_page(ui, wallpaper ? "选择壁纸" : "图库");
    if (wallpaper) ui->page_back = app_open_theme;
    gallery_page_t *page = calloc(1, sizeof(*page));
    if (!page) { app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF8888, NULL); return; }
    ui->page_context = page;
    ui->page_cleanup = gallery_cleanup;
    page->wallpaper = wallpaper;
    if (wallpaper) ui->page_can_exit = wallpaper_can_exit;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    app_catalog_panel(content);
    lv_obj_set_style_pad_all(content, 19, 0);
    app_ui_add_text(content, wallpaper ? "TF 卡图片" : "全部图片", LV_ALIGN_TOP_LEFT, 2, 1, 0xEAF2FF, app_ui_chinese_font_size(24));
    page->source_label = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 3, 33, 0x90C0FF, NULL);
    page->refresh = gallery_button(content, LV_SYMBOL_REFRESH, 354, 3, 54, gallery_action, ui, 0);
    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_pos(list, 0, 59);
    lv_obj_set_size(list, 408, 268);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    app_scroll_list_init(&page->rows, list, 59, create_gallery_row, bind_gallery_row, ui);
    page->summary = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 333, 0xB7CCE8, NULL);
    lv_obj_set_width(page->summary, 408);
    lv_label_set_long_mode(page->summary, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(page->summary, LV_TEXT_ALIGN_CENTER, 0);
    app_ui_set_timer(ui, gallery_timer, 250);
    gallery_refresh(ui);
}

void app_open_gallery(app_ui_t *ui) { open_gallery(ui, false); }
void app_open_wallpaper_picker(app_ui_t *ui) { open_gallery(ui, true); }

void app_open_image_path(app_ui_t *ui, const char *path)
{
    lv_obj_t *content = app_ui_begin_page(ui, "图片");
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    app_catalog_panel(content);
    lv_obj_set_style_pad_all(content, 19, 0);
    gallery_page_t *page = calloc(1, sizeof(*page));
    if (page) page->entries = calloc(1, sizeof(*page->entries));
    if (!page || !page->entries) {
        free(page);
        app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF8888, NULL);
        return;
    }
    ui->page_context = page;
    ui->page_cleanup = gallery_cleanup;
    page->count = 1;
    strlcpy(page->entries[0].path, path, sizeof(page->entries[0].path));
    page->media = gallery_media(ui, false);
    if (!gallery_selection_ready(ui, page, page->entries[0].path)) {
        app_ui_add_text(content, "图片来源已断开", LV_ALIGN_CENTER, 0, 0, 0xFF8888, NULL);
        return;
    }
    app_ui_set_timer(ui, gallery_timer, 250);
    gallery_show_image(ui, 0);
}
