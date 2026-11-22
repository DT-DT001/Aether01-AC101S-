#include "app_ui_internal.h"
#include "app_scroll_list.h"
#include "app_text.h"
#include "aether_image.h"
#include "aether_media_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#define GALLERY_CATALOG_LIMIT 4096U
#define GALLERY_IMAGE_WIDTH 408U
#define GALLERY_IMAGE_HEIGHT 276U
#define GALLERY_PHOTOS AETHER_STORAGE_MOUNT_POINT "/DCIM"

typedef struct {
    atomic_uint users;
    atomic_bool cancelled;
    atomic_bool done;
    aether_storage_handle_t storage;
    uint32_t generation;
    char path[192];
    esp_err_t error;
    uint16_t *pixels, width, height;
} gallery_job_t;

typedef struct {
    char directory[192];
    app_io_entry_t *entries;
    size_t count, selected;
    uint32_t generation;
    bool ready, loading;
    app_scroll_list_t rows;
    lv_obj_t *path_label, *summary, *up, *photos, *refresh;
    lv_obj_t *viewer, *image, *image_label, *older, *newer;
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

static bool gallery_ready(app_ui_t *ui)
{
    return aether_storage_is_mounted(ui->storage) && !app_io_usb_storage_blocked(ui->io) &&
           !app_ui_input_blocked(ui);
}

static bool read_available(const gallery_job_t *job, const aether_media_fs_lease_t *lease)
{
    return !atomic_load(&job->cancelled) &&
           aether_storage_generation(job->storage) == job->generation &&
           aether_media_fs_available(lease);
}

static esp_err_t read_jpeg(FILE *file, size_t bytes, gallery_job_t *job,
                          const aether_media_fs_lease_t *lease)
{
    if (bytes > AETHER_IMAGE_MAX_JPEG_BYTES) return ESP_ERR_INVALID_SIZE;
    uint8_t *data = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) return ESP_ERR_NO_MEM;
    size_t offset = 0;
    esp_err_t error = ESP_OK;
    while (offset < bytes && read_available(job, lease)) {
        const size_t count = bytes - offset < 32768U ? bytes - offset : 32768U;
        if (fread(data + offset, 1, count, file) != count) {
            error = ESP_FAIL;
            break;
        }
        offset += count;
    }
    if (error == ESP_OK && !read_available(job, lease)) error = ESP_ERR_INVALID_STATE;
    aether_image_decoder_t *decoder = NULL;
    if (error == ESP_OK) error = aether_image_decoder_new(&decoder);
    if (error == ESP_OK) {
        error = aether_image_decode_jpeg(decoder, data, bytes, job->pixels,
                                        GALLERY_IMAGE_WIDTH, GALLERY_IMAGE_HEIGHT,
                                        &job->width, &job->height);
    }
    aether_image_decoder_del(decoder);
    free(data);
    return error;
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)read_le16(p) | (uint32_t)read_le16(p + 2) << 16;
}

static esp_err_t read_bmp(FILE *file, size_t bytes, gallery_job_t *job,
                         const aether_media_fs_lease_t *lease)
{
    uint8_t header[54];
    if (bytes < sizeof(header) || fread(header, 1, sizeof(header), file) != sizeof(header))
        return ESP_ERR_INVALID_RESPONSE;
    const uint32_t dib_size = read_le32(header + 14);
    const uint32_t start = read_le32(header + 10);
    const unsigned bits = read_le16(header + 28);
    const int32_t width = (int32_t)read_le32(header + 18);
    const int32_t raw_height = (int32_t)read_le32(header + 22);
    if (header[0] != 'B' || header[1] != 'M' || dib_size < 40 || read_le16(header + 26) != 1 ||
        read_le32(header + 30) != 0 || (bits != 24 && bits != 32)) return ESP_ERR_NOT_SUPPORTED;
    if (width <= 0 || width > 8192 || !raw_height || raw_height < -8192 || raw_height > 8192 ||
        start < 14U + (uint64_t)dib_size) return ESP_ERR_INVALID_SIZE;

    const unsigned height = (unsigned)(raw_height < 0 ? -raw_height : raw_height);
    const unsigned pixel_bytes = bits / 8;
    const unsigned row_bytes = ((unsigned)width * pixel_bytes + 3U) & ~3U;
    if (start + (uint64_t)row_bytes * height > bytes) return ESP_ERR_INVALID_SIZE;
    unsigned fit_width = (unsigned)width < GALLERY_IMAGE_WIDTH ? (unsigned)width : GALLERY_IMAGE_WIDTH;
    unsigned fit_height = height * fit_width / (unsigned)width;
    if (fit_height > GALLERY_IMAGE_HEIGHT) {
        fit_height = GALLERY_IMAGE_HEIGHT;
        fit_width = (unsigned)width * fit_height / height;
    }
    if (!fit_width) fit_width = 1;
    if (!fit_height) fit_height = 1;
    uint8_t *row = heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!row) return ESP_ERR_NO_MEM;

    esp_err_t error = ESP_OK;
    const unsigned x_step = ((unsigned)width << 16) / fit_width;
    const unsigned y_step = (height << 16) / fit_height;
    unsigned source_y = y_step / 2;
    for (unsigned y = 0; y < fit_height; ++y, source_y += y_step) {
        if (!read_available(job, lease)) { error = ESP_ERR_INVALID_STATE; break; }
        /* Read physical rows forwards even for bottom-up BMPs. */
        const unsigned offset = start + (source_y >> 16) * row_bytes;
        if (fseek(file, (long)offset, SEEK_SET) || fread(row, 1, row_bytes, file) != row_bytes) {
            error = ESP_FAIL;
            break;
        }
        const unsigned dest_y = raw_height < 0 ? y : fit_height - y - 1;
        uint16_t *dest = job->pixels + dest_y * fit_width;
        unsigned source_x = x_step / 2;
        for (unsigned x = 0; x < fit_width; ++x, source_x += x_step) {
            const uint8_t *src = row + (source_x >> 16) * pixel_bytes;
            dest[x] = (uint16_t)((src[2] >> 3) << 11 | (src[1] >> 2) << 5 | src[0] >> 3);
        }
    }
    free(row);
    job->width = (uint16_t)fit_width;
    job->height = (uint16_t)fit_height;
    return error;
}

static void job_release(gallery_job_t *job)
{
    if (atomic_fetch_sub(&job->users, 1) == 1) {
        free(job->pixels);
        free(job);
    }
}

static esp_err_t decode_file(gallery_job_t *job, const aether_media_fs_lease_t *lease)
{
    if (!read_available(job, lease)) return ESP_ERR_INVALID_STATE;
    FILE *file = fopen(job->path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    esp_err_t error = ESP_ERR_INVALID_SIZE;
    const long bytes = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    if (bytes > 0 && fseek(file, 0, SEEK_SET) == 0) {
        job->pixels = heap_caps_malloc(GALLERY_IMAGE_WIDTH * GALLERY_IMAGE_HEIGHT * sizeof(uint16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const char *ext = strrchr(job->path, '.');
        if (!job->pixels) error = ESP_ERR_NO_MEM;
        else if (ext && !strcasecmp(ext, ".bmp")) error = read_bmp(file, (size_t)bytes, job, lease);
        else error = read_jpeg(file, (size_t)bytes, job, lease);
    }
    fclose(file);
    return error;
}

static void image_decode_task(void *arg)
{
    gallery_job_t *job = arg;
    aether_media_fs_lease_t lease = {0};
    if (atomic_load(&job->cancelled)) job->error = ESP_ERR_INVALID_STATE;
    else {
        job->error = aether_media_fs_acquire(job->storage, job->path, &lease);
        if (job->error == ESP_OK) {
            job->error = decode_file(job, &lease);
            aether_media_fs_release(&lease);
        }
    }
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

static void gallery_close_viewer(gallery_page_t *page)
{
    if (page->job) {
        atomic_store(&page->job->cancelled, true);
        job_release(page->job);
        page->job = NULL;
    }
    gallery_drop_pixels(page);
    if (page->viewer) {
        lv_obj_delete(page->viewer);
        page->viewer = NULL;
        page->image = NULL;
    }
}

static void gallery_cleanup(app_ui_t *ui)
{
    gallery_page_t *page = ui->page_context;
    if (page) {
        app_scroll_list_deinit(&page->rows);
        gallery_close_viewer(page);
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
    lv_obj_set_size(button, width, 40);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x20334F), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x314C70), LV_STATE_PRESSED);
    lv_obj_set_user_data(button, (void *)action);
    lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED, ui);
    app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0, 0xEAF2FF, NULL);
    return button;
}

static size_t adjacent_photo(const gallery_page_t *page, int direction)
{
    int index = (int)page->selected + direction;
    for (; index >= 0 && (size_t)index < page->count; index += direction) {
        if (!page->entries[index].directory) return (size_t)index;
    }
    return SIZE_MAX;
}

static void viewer_controls(gallery_page_t *page)
{
    set_enabled(page->older, !page->job && adjacent_photo(page, -1) != SIZE_MAX);
    set_enabled(page->newer, !page->job && adjacent_photo(page, 1) != SIZE_MAX);
}

static void viewer_action(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    gallery_page_t *page = ui->page_context;
    const intptr_t action = (intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (!page || !page->viewer) return;
    if (!action) { gallery_close_viewer(page); return; }
    const size_t index = adjacent_photo(page, (int)action);
    if (!page->job && index != SIZE_MAX) gallery_show_image(ui, index);
}

static void gallery_show_image(app_ui_t *ui, size_t index)
{
    gallery_page_t *page = ui->page_context;
    if (!page || page->job || index >= page->count || page->entries[index].directory ||
        !gallery_ready(ui) || aether_storage_generation(ui->storage) != page->generation) return;
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
        gallery_button(page->viewer, "返回列表", 8, 307, 120, viewer_action, ui, 0);
        page->older = gallery_button(page->viewer, "上一张", 144, 307, 120, viewer_action, ui, -1);
        page->newer = gallery_button(page->viewer, "下一张", 280, 307, 120, viewer_action, ui, 1);
    }
    page->selected = index;
    gallery_drop_pixels(page);
    if (atomic_exchange(&decode_running, true)) {
        app_ui_label_text(page->image_label, "上次读图正在结束，返回列表后重试");
        viewer_controls(page);
        return;
    }
    gallery_job_t *job = calloc(1, sizeof(*job));
    if (job) {
        atomic_init(&job->users, 2);
        atomic_init(&job->cancelled, false);
        atomic_init(&job->done, false);
        job->storage = ui->storage;
        job->generation = page->generation;
        strlcpy(job->path, page->entries[index].path, sizeof(job->path));
        if (xTaskCreate(image_decode_task, "gallery", 4096, job, 3, NULL) != pdPASS) {
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
        page->pixels = job->pixels;
        job->pixels = NULL;
        page->descriptor = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                       .w = job->width, .h = job->height, .stride = job->width * 2U},
            .data_size = (uint32_t)job->width * job->height * 2U,
            .data = (const uint8_t *)page->pixels,
        };
        lv_image_set_src(page->image, &page->descriptor);
        lv_obj_set_pos(page->image, (GALLERY_IMAGE_WIDTH - job->width) / 2,
                        (GALLERY_IMAGE_HEIGHT - job->height) / 2);
        char name[192];
        strlcpy(name, file_name(job->path), sizeof(name));
        app_text_sanitize_utf8(name, strlen(name));
        app_ui_label_text(page->image_label, name);
    } else {
        const char *reason = job->error == ESP_ERR_NOT_SUPPORTED ? "仅支持基线 JPEG 或 24/32 位未压缩 BMP" :
                             job->error == ESP_ERR_NO_MEM ? "内存不足" :
                             job->error == ESP_ERR_INVALID_SIZE ? "图片太大或尺寸无效" :
                             job->error == ESP_ERR_NOT_FOUND ? "文件已移除" : "文件损坏或读取失败";
        app_ui_label_fmt(page->image_label, "无法打开图片 · %s", reason);
    }
    job_release(job);
    viewer_controls(page);
}

static void catalog_controls(gallery_page_t *page)
{
    const bool ready = page->ready && !page->loading;
    set_enabled(page->up, ready && strcmp(page->directory, AETHER_STORAGE_MOUNT_POINT));
    set_enabled(page->photos, ready && strcmp(page->directory, GALLERY_PHOTOS));
    set_enabled(page->refresh, ready);
}

static void gallery_entry_clicked(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    gallery_page_t *page = ui->page_context;
    const size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (!page || page->loading || page->viewer || index >= page->count ||
        !gallery_ready(ui) || aether_storage_generation(ui->storage) != page->generation) return;
    if (page->entries[index].directory) {
        strlcpy(page->directory, page->entries[index].path, sizeof(page->directory));
        gallery_refresh(ui);
    } else {
        gallery_show_image(ui, index);
    }
}

static lv_obj_t *create_gallery_row(lv_obj_t *parent, void *context)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, LV_PCT(100), 47);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x18243A), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x314C70), LV_STATE_PRESSED);
    app_ui_add_text(button, "", LV_ALIGN_LEFT_MID, 0, 0, 0x90C0FF, &lv_font_montserrat_16);
    lv_obj_t *label = app_ui_add_text(button, "", LV_ALIGN_LEFT_MID, 30, 0, 0xEAF2FF, NULL);
    lv_obj_set_width(label, 340);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_add_event_cb(button, gallery_entry_clicked, LV_EVENT_CLICKED, context);
    return button;
}

static void bind_gallery_row(lv_obj_t *row, size_t index, void *context)
{
    app_ui_t *ui = context;
    const gallery_page_t *page = ui->page_context;
    const app_io_entry_t *entry = &page->entries[index];
    char name[192];
    strlcpy(name, file_name(entry->path), sizeof(name));
    app_text_sanitize_utf8(name, strlen(name));
    app_ui_label_text(lv_obj_get_child(row, 0), entry->directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_IMAGE);
    app_ui_label_text(lv_obj_get_child(row, 1), name);
}

static void gallery_catalog_ready(app_ui_t *ui, const app_io_result_t *result)
{
    gallery_page_t *page = ui->page_context;
    page->loading = false;
    if (aether_storage_generation(ui->storage) != page->generation) {
        strlcpy(page->directory, AETHER_STORAGE_MOUNT_POINT, sizeof(page->directory));
        gallery_refresh(ui);
        return;
    }
    if (!gallery_ready(ui)) {
        gallery_refresh(ui);
        return;
    }
    if (result->error != ESP_OK) {
        app_ui_label_text(page->summary, result->error == ESP_ERR_NOT_FOUND ?
                          "此文件夹尚未创建" : "读取目录失败，点击刷新重试");
        catalog_controls(page);
        return;
    }
    if (result->count) {
        page->entries = heap_caps_malloc(result->count * sizeof(*page->entries), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!page->entries) {
            app_ui_label_text(page->summary, "内存不足，无法显示目录");
            catalog_controls(page);
            return;
        }
        memcpy(page->entries, result->entries, result->count * sizeof(*page->entries));
    }
    page->count = result->count;
    app_scroll_list_set_count(&page->rows, page->count);
    if (result->total_count > page->count) {
        app_ui_label_fmt(page->summary, "显示前 %u / %u 项", (unsigned)page->count,
                         (unsigned)result->total_count);
    } else if (page->count) {
        app_ui_label_fmt(page->summary, "共 %u 项", (unsigned)page->count);
    } else {
        app_ui_label_text(page->summary, "此处没有图片");
    }
    catalog_controls(page);
}

static void gallery_refresh(app_ui_t *ui)
{
    gallery_page_t *page = ui->page_context;
    if (!page || page->loading || ui->io_complete) return;
    gallery_close_viewer(page);
    app_scroll_list_set_count(&page->rows, 0);
    free(page->entries);
    page->entries = NULL;
    page->count = 0;
    app_ui_label_text(page->path_label, page->directory);
    page->generation = aether_storage_generation(ui->storage);
    page->ready = gallery_ready(ui);
    if (!page->ready) {
        app_ui_label_text(page->summary, app_io_usb_storage_blocked(ui->io) ?
                          "请先退出 USB 文件传输" : "请插入可读取的 TF 卡");
        catalog_controls(page);
        return;
    }
    page->loading = true;
    app_ui_label_text(page->summary, "正在读取目录…");
    catalog_controls(page);
    app_io_request_t request = {
        .operation = APP_IO_CATALOG, .filter = APP_IO_IMAGES,
        .limit = GALLERY_CATALOG_LIMIT,
    };
    strlcpy(request.path, page->directory, sizeof(request.path));
    app_ui_request_io_ex(ui, &request, gallery_catalog_ready);
}

static void gallery_action(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    gallery_page_t *page = ui->page_context;
    const intptr_t action = (intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    if (!page || page->loading || page->viewer) return;
    switch (action) {
    case 1: {
        char *slash = strrchr(page->directory, '/');
        if (slash && slash > page->directory) *slash = 0;
        break;
    }
    case 2:
        strlcpy(page->directory, GALLERY_PHOTOS, sizeof(page->directory));
        break;
    default:
        break;
    }
    gallery_refresh(ui);
}

static void gallery_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    gallery_page_t *page = ui->page_context;
    if (!page) return;
    const uint32_t generation = aether_storage_generation(ui->storage);
    if (page->ready != gallery_ready(ui) || page->generation != generation) {
        if (page->generation != generation)
            strlcpy(page->directory, AETHER_STORAGE_MOUNT_POINT, sizeof(page->directory));
        gallery_refresh(ui);
    } else if (page->viewer) {
        gallery_present(page);
    }
    lv_timer_set_period(timer, page->job ? 40 : 250);
}

void app_open_gallery(app_ui_t *ui)
{
    lv_obj_t *content = app_ui_begin_page(ui, "图库");
    gallery_page_t *page = calloc(1, sizeof(*page));
    if (!page) { app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF8888, NULL); return; }
    ui->page_context = page;
    ui->page_cleanup = gallery_cleanup;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    strlcpy(page->directory, AETHER_STORAGE_MOUNT_POINT, sizeof(page->directory));
    page->up = gallery_button(content, "上级目录", 0, 0, 128, gallery_action, ui, 1);
    page->photos = gallery_button(content, "拍摄目录", 140, 0, 128, gallery_action, ui, 2);
    page->refresh = gallery_button(content, "刷新", 280, 0, 128, gallery_action, ui, 0);
    page->path_label = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 1, 49, 0x90C0FF, NULL);
    lv_obj_set_width(page->path_label, 405);
    lv_label_set_long_mode(page->path_label, LV_LABEL_LONG_DOT);
    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_pos(list, 0, 76);
    lv_obj_set_size(list, 408, 251);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    app_scroll_list_init(&page->rows, list, 53, create_gallery_row, bind_gallery_row, ui);
    page->summary = app_ui_add_text(content, "", LV_ALIGN_TOP_LEFT, 0, 333, 0xB7CCE8, NULL);
    lv_obj_set_width(page->summary, 408);
    lv_label_set_long_mode(page->summary, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(page->summary, LV_TEXT_ALIGN_CENTER, 0);
    app_ui_set_timer(ui, gallery_timer, 250);
    gallery_refresh(ui);
}
