#include "app_ui_internal.h"
#include <assert.h>
#include <stdatomic.h>

#undef AETHER_STORAGE_MOUNT_POINT
#define AETHER_STORAGE_MOUNT_POINT "photos"
void app_open_gallery(app_ui_t *);
#include "../../main/apps/app_gallery.c"

extern const lv_font_t font_16, font_24;
static uint16_t draw_buffer[480 * 40];
static uint16_t framebuffer[480 * 480];
static app_io_entry_t fixtures[GALLERY_CATALOG_LIMIT];
static app_io_request_t request;
static bool mounted[] = {true, false};
static bool usb_blocked, pressed;
static lv_point_t point;
static unsigned catalog_queries, catalog_cancels, leases, task_creates, previous_page_calls;
static uint32_t generations[] = {1, 1};
static void (*worker)(void *);
static void *worker_arg;

static unsigned fixture_root(const char *path) { return !strncmp(path, "usbphotos", 9); }
bool aether_media_fs_path_ready(aether_storage_handle_t storage, const char *path)
{ assert(storage); return mounted[fixture_root(path)]; }
uint32_t aether_media_fs_generation(aether_storage_handle_t storage, const char *path)
{ assert(storage); return generations[fixture_root(path)]; }
bool app_io_usb_storage_blocked(app_io_handle_t io) { assert(io); return usb_blocked; }
bool app_ui_input_blocked(const app_ui_t *ui) { assert(ui); return false; }
void *heap_caps_malloc(size_t size, unsigned caps) { assert(caps == 3); return malloc(size); }
esp_err_t aether_media_fs_acquire(aether_storage_handle_t storage, const char *path, aether_media_fs_lease_t *lease)
{
    assert(storage && path && !lease->held);
    lease->source = fixture_root(path);
    if (!mounted[lease->source]) return ESP_ERR_INVALID_STATE;
    lease->held = true; ++leases; return ESP_OK;
}
bool aether_media_fs_available(const aether_media_fs_lease_t *lease)
{ return lease->held && mounted[lease->source]; }
void aether_media_fs_release(aether_media_fs_lease_t *lease)
{ assert(lease->held && leases); lease->held = false; --leases; }
esp_err_t aether_image_decoder_new(aether_image_decoder_t **decoder)
{ *decoder = NULL; return ESP_ERR_NOT_SUPPORTED; }
void aether_image_decoder_del(aether_image_decoder_t *decoder) { assert(!decoder); }
esp_err_t aether_image_decode_jpeg(aether_image_decoder_t *decoder, const uint8_t *source,
    size_t bytes, uint16_t *pixels, uint16_t max_width, uint16_t max_height,
    uint16_t *width, uint16_t *height)
{
    (void)decoder; (void)source; (void)bytes; (void)pixels;
    (void)max_width; (void)max_height; (void)width; (void)height;
    return ESP_ERR_NOT_SUPPORTED;
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                unsigned priority, void *handle)
{
    assert(!worker && name && stack && priority && !handle);
    worker = fn;
    worker_arg = arg;
    ++task_creates;
    return pdPASS;
}
void vTaskDelete(void *task) { assert(!task); }

size_t strlcpy(char *out, const char *text, size_t size)
{ if (size) snprintf(out, size, "%s", text); return strlen(text); }
const lv_font_t *app_ui_chinese_font(void) { return &font_16; }
const lv_font_t *app_ui_chinese_font_size(unsigned size) { return size >= 24 ? &font_24 : &font_16; }
void app_ui_label_text(lv_obj_t *label, const char *text) { lv_label_set_text(label, text); }
void app_ui_label_fmt(lv_obj_t *label, const char *format, ...)
{
    char out[256];
    va_list args;
    va_start(args, format);
    vsnprintf(out, sizeof(out), format, args);
    va_end(args);
    lv_label_set_text(label, out);
}
lv_obj_t *app_ui_add_text(lv_obj_t *parent, const char *text, lv_align_t align,
                         int32_t x, int32_t y, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font ? font : &font_16, 0);
    lv_obj_align(label, align, x, y);
    return label;
}
static void header_back(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    if (lv_obj_get_parent(lv_event_get_current_target_obj(event)) == ui->screen && ui->page_back)
        ui->page_back(ui);
}
lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title)
{
    if (ui->timer) { lv_timer_delete(ui->timer); ui->timer = NULL; }
    if (ui->page_cleanup) { ui->page_cleanup(ui); ui->page_cleanup = NULL; }
    ui->page_back = NULL;
    ++ui->page_generation;
    ui->io_complete = NULL;
    lv_obj_t *old = ui->screen;
    ui->screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui->screen, lv_color_hex(0x101321), 0);
    lv_obj_t *back = lv_button_create(ui->screen);
    lv_obj_set_size(back, 48, 44);
    lv_obj_set_pos(back, 14, 12);
    lv_obj_add_event_cb(back, header_back, LV_EVENT_CLICKED, ui);
    app_ui_add_text(back, LV_SYMBOL_LEFT, LV_ALIGN_CENTER, 0, 0, 0xBFD7FF, &lv_font_montserrat_16);
    app_ui_add_text(ui->screen, title, LV_ALIGN_TOP_MID, 0, 20, 0xEAF2FF, &font_24);
    ui->content = lv_obj_create(ui->screen);
    lv_obj_set_size(ui->content, 448, 394);
    lv_obj_align(ui->content, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_pad_all(ui->content, 20, 0);
    lv_obj_set_style_radius(ui->content, 24, 0);
    lv_screen_load(ui->screen);
    if (old) lv_obj_delete(old);
    return ui->content;
}
void app_ui_set_timer(app_ui_t *ui, lv_timer_cb_t callback, uint32_t period)
{ ui->timer = lv_timer_create(callback, period, ui); }
void app_ui_request_io_ex(app_ui_t *ui, const app_io_request_t *candidate,
                          void (*complete)(app_ui_t *, const app_io_result_t *))
{
    assert(!ui->io_complete);
    request = *candidate;
    assert(request.operation == APP_IO_CATALOG && request.filter == APP_IO_IMAGES &&
           request.limit == GALLERY_CATALOG_LIMIT && request.offset == 0 && !request.path[0]);
    ++catalog_queries;
    ui->io_complete = complete;
}

void app_io_cancel(app_io_handle_t io, uint32_t epoch)
{ assert(io && epoch); ++catalog_cancels; }

static void finish_result(app_ui_t *ui, esp_err_t error, size_t count, size_t total, bool partial)
{
    assert(ui->io_complete);
    void (*complete)(app_ui_t *, const app_io_result_t *) = ui->io_complete;
    ui->io_complete = NULL;
    complete(ui, &(app_io_result_t){.entries = fixtures, .count = count, .total_count = total,
                                   .error = error, .truncated = partial});
}

static void finish_catalog(app_ui_t *ui, size_t count, size_t total)
{ finish_result(ui, ESP_OK, count, total, total > count); }
static void run_worker(void)
{
    assert(worker);
    void (*function)(void *) = worker;
    worker = NULL;
    function(worker_arg);
    assert(!leases && !atomic_load(&decode_running));
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    assert(area->x1 >= 0 && area->x2 < 480 && area->y1 >= 0 && area->y2 < 480 && pixels);
    const uint16_t *source = (const uint16_t *)pixels;
    const size_t width = (size_t)(area->x2 - area->x1 + 1);
    for (int32_t y = area->y1; y <= area->y2; ++y, source += width)
        memcpy(framebuffer + y * 480 + area->x1, source, width * sizeof(*source));
    lv_display_flush_ready(display);
}

static void snapshot(lv_display_t *display, const char *path)
{
    lv_refr_now(display);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n480 480\n255\n");
    for (size_t i = 0; i < sizeof(framebuffer) / sizeof(framebuffer[0]); ++i) {
        const uint16_t pixel = framebuffer[i];
        const uint8_t rgb[] = {(pixel >> 11) * 255 / 31, ((pixel >> 5) & 63) * 255 / 63, (pixel & 31) * 255 / 31};
        assert(fwrite(rgb, 1, sizeof(rgb), file) == sizeof(rgb));
    }
    assert(!fclose(file));
}
static void input(lv_indev_t *indev, lv_indev_data_t *data)
{ (void)indev; data->point = point; data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED; }
static void advance(unsigned ms)
{ for (unsigned i = 0; i < ms; ++i) { lv_tick_inc(1); lv_timer_handler(); } }
static void tap(lv_obj_t *object)
{
    lv_obj_update_layout(lv_obj_get_screen(object));
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    point = (lv_point_t){(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    pressed = true;
    advance(55);
    pressed = false;
    advance(55);
}

static void previous_page(app_ui_t *ui) { assert(ui); ++previous_page_calls; }

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(480, 480);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, input);
    app_ui_t ui = {.storage = (void *)1, .io = (void *)1};
    for (size_t i = 0; i < GALLERY_CATALOG_LIMIT; ++i)
        snprintf(fixtures[i].path, sizeof(fixtures[i].path), "photos/DCIM/album/item-%04zu.jpg", i);
    strlcpy(fixtures[GALLERY_CATALOG_LIMIT - 1].path, "photos/tail.bmp", sizeof(fixtures[0].path));

    app_open_gallery(&ui);
    gallery_page_t *page = ui.page_context;
    lv_obj_t *screen = ui.screen;
    lv_obj_t *back = lv_obj_get_child(screen, 0);
    ui.page_back = previous_page;
    assert(page->loading && !request.path[0]);
    finish_catalog(&ui, GALLERY_CATALOG_LIMIT, GALLERY_CATALOG_LIMIT + 1);
    advance(50);
    assert(page->rows.row_count < APP_SCROLL_LIST_ROWS);
    assert(lv_obj_get_child_count(page->rows.viewport) == page->rows.row_count + 1);
    assert(strstr(lv_label_get_text(page->summary), "4096 / 4097"));
    assert(catalog_queries == 1 && !task_creates);
    snapshot(display, "gallery-catalog.ppm");
    lv_obj_scroll_to_y(page->rows.viewport, GALLERY_CATALOG_LIMIT * page->rows.row_step, LV_ANIM_OFF);
    advance(50);
    const size_t tail = GALLERY_CATALOG_LIMIT - 1;
    const size_t tail_slot = tail % page->rows.row_count;
    assert(page->rows.indices[tail_slot] == tail);
    const int32_t saved_scroll = lv_obj_get_scroll_y(page->rows.viewport);
    tap(page->rows.rows[tail_slot]);
    assert(page->selected == tail && page->job && task_creates == 1);
    assert(ui.page_back == gallery_viewer_back && lv_obj_has_flag(page->rows.viewport, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_child_count(page->viewer) == 4 && lv_obj_is_visible(back));
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(page->older, 0)), LV_SYMBOL_PREV));
    run_worker();
    advance(80);
    assert(!page->job && page->pixels && page->descriptor.header.w == 2 && page->descriptor.header.h == 2);
    assert(!strcmp(lv_label_get_text(page->image_label), "4096 / 4096  ·  tail.bmp"));
    assert(lv_obj_has_state(page->newer, LV_STATE_DISABLED));
    tap(page->older);
    assert(page->selected == tail - 1 && page->job && task_creates == 2);
    run_worker();
    advance(80);
    assert(!page->job && !page->pixels);
    tap(back);
    advance(50);
    assert(ui.screen == screen && !page->viewer && ui.page_back == previous_page && !previous_page_calls);
    assert(lv_obj_get_scroll_y(page->rows.viewport) == saved_scroll);
    tap(back);
    assert(previous_page_calls == 1);
    tap(page->rows.rows[tail_slot]);
    assert(page->job && task_creates == 3);
    app_ui_begin_page(&ui, "桌面");
    run_worker();
    assert(!ui.page_context && !ui.timer);
    puts("PASS AC101S Gallery: 4096 photo-only entries, bounded row pool, real BMP viewing, adjacent photo, Back restores scroll, and decode cancellation");

    app_open_gallery(&ui);
    page = ui.page_context;
    const unsigned before_attach = catalog_queries;
    mounted[1] = true;
    ++generations[1];
    advance(300);
    assert(page->loading && catalog_cancels == 1 && catalog_queries == before_attach + 1);
    assert(!strcmp(lv_label_get_text(page->source_label), "TF 卡 · USB"));
    strlcpy(fixtures[0].path, "usbphotos/DCIM/Camera/tail.bmp", sizeof(fixtures[0].path));
    strlcpy(fixtures[1].path, "photos/tail.bmp", sizeof(fixtures[1].path));
    finish_catalog(&ui, 2, 2);
    advance(50);
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(page->rows.rows[0], 2)), "USB · DCIM/Camera"));
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(page->rows.rows[1], 2)), "TF 卡"));
    tap(page->rows.rows[0]);
    assert(page->job && page->job->generation == generations[1]);
    run_worker();
    advance(80);
    assert(page->pixels);
    const unsigned before_remove = catalog_queries;
    mounted[1] = false;
    ++generations[1];
    advance(300);
    assert(!page->viewer && !page->pixels && page->loading && catalog_queries == before_remove + 1);
    finish_result(&ui, ESP_FAIL, 0, 0, false);
    assert(strstr(lv_label_get_text(page->summary), "扫描图片失败"));
    tap(page->refresh);
    finish_catalog(&ui, 0, 0);
    assert(strstr(lv_label_get_text(page->summary), "没有可显示的图片"));

    mounted[0] = false;
    mounted[1] = true;
    ++generations[0];
    ++generations[1];
    advance(300);
    assert(page->loading && !strcmp(lv_label_get_text(page->source_label), "USB"));
    const unsigned before_usb = catalog_queries;
    usb_blocked = true;
    advance(300);
    assert(!page->rows.count && !page->media.sources && !page->loading && !ui.io_complete &&
           catalog_queries == before_usb && catalog_cancels == 2);
    assert(strstr(lv_label_get_text(page->summary), "退出 USB 文件传输"));
    usb_blocked = false;
    advance(300);
    assert(page->loading && catalog_queries == before_usb + 1);
    finish_result(&ui, ESP_OK, 1, 1, true);
    assert(strstr(lv_label_get_text(page->summary), "部分目录未读取"));
    for (unsigned i = 0; i < 30; ++i) {
        app_open_gallery(&ui);
        finish_catalog(&ui, 1, 1);
        advance(35);
        assert(lv_mem_test() == LV_RESULT_OK);
    }
    app_ui_begin_page(&ui, "桌面");
    lv_indev_delete(pointer);
    lv_display_delete(display);
    lv_deinit();
    puts("PASS AC101S Gallery: source labels, USB playback/removal, scan replacement, export gate, partial/empty/error states, and 30 rebuilds");
}
