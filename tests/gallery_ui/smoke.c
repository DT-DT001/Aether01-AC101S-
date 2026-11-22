#include "app_ui_internal.h"
#include <assert.h>
#include <stdatomic.h>

#undef AETHER_STORAGE_MOUNT_POINT
#define AETHER_STORAGE_MOUNT_POINT "photos"
uint32_t aether_storage_generation(aether_storage_handle_t);
bool app_ui_input_blocked(const app_ui_t *);
void app_open_gallery(app_ui_t *);
#include "../../main/apps/app_gallery.c"

extern const lv_font_t font_16, font_24;
static uint16_t draw_buffer[480 * 40];
static app_io_entry_t fixtures[GALLERY_CATALOG_LIMIT];
static app_io_request_t request;
static bool mounted = true, usb_blocked, pressed;
static lv_point_t point;
static unsigned catalog_queries, leases, task_creates;
static uint32_t generation = 1;
static void (*worker)(void *);
static void *worker_arg;

bool aether_storage_is_mounted(aether_storage_handle_t storage) { assert(storage); return mounted; }
uint32_t aether_storage_generation(aether_storage_handle_t storage) { assert(storage); return generation; }
bool app_io_usb_storage_blocked(app_io_handle_t io) { assert(io); return usb_blocked; }
bool app_ui_input_blocked(const app_ui_t *ui) { assert(ui); return false; }
void *heap_caps_malloc(size_t size, unsigned caps) { assert(caps == 3); return malloc(size); }
esp_err_t aether_media_fs_acquire(aether_storage_handle_t storage, const char *path, aether_media_fs_lease_t *lease)
{ assert(storage && path && !lease->held); lease->held = true; ++leases; return ESP_OK; }
bool aether_media_fs_available(const aether_media_fs_lease_t *lease) { return mounted && lease->held; }
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
lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title)
{
    if (ui->timer) { lv_timer_delete(ui->timer); ui->timer = NULL; }
    if (ui->page_cleanup) { ui->page_cleanup(ui); ui->page_cleanup = NULL; }
    ui->io_complete = NULL;
    lv_obj_t *old = ui->screen;
    ui->screen = lv_obj_create(NULL);
    app_ui_add_text(ui->screen, title, LV_ALIGN_TOP_MID, 0, 14, 0xFFFFFF, &font_24);
    ui->content = lv_obj_create(ui->screen);
    lv_obj_set_size(ui->content, 448, 394);
    lv_obj_align(ui->content, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_pad_all(ui->content, 20, 0);
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
           request.limit == GALLERY_CATALOG_LIMIT && request.offset == 0);
    ++catalog_queries;
    ui->io_complete = complete;
}

static void finish_catalog(app_ui_t *ui, size_t count, size_t total)
{
    assert(ui->io_complete);
    void (*complete)(app_ui_t *, const app_io_result_t *) = ui->io_complete;
    ui->io_complete = NULL;
    complete(ui, &(app_io_result_t){.entries = fixtures, .count = count, .total_count = total});
}
static void run_worker(void)
{
    assert(worker);
    void (*function)(void *) = worker;
    worker = NULL;
    function(worker_arg);
    assert(!leases && !atomic_load(&decode_running));
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ assert(area->x1 >= 0 && area->x2 < 480 && area->y1 >= 0 && area->y2 < 480 && pixels); lv_display_flush_ready(display); }
static void input(lv_indev_t *indev, lv_indev_data_t *data)
{ (void)indev; data->point = point; data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED; }
static void advance(unsigned ms)
{ for (unsigned i = 0; i < ms; ++i) { lv_tick_inc(1); lv_timer_handler(); } }
static void tap(lv_obj_t *object)
{
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    point = (lv_point_t){(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    pressed = true;
    advance(55);
    pressed = false;
    advance(55);
}

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
        snprintf(fixtures[i].path, sizeof(fixtures[i].path), "photos/item-%04zu.jpg", i);
    fixtures[GALLERY_CATALOG_LIMIT - 2].directory = true;
    strlcpy(fixtures[GALLERY_CATALOG_LIMIT - 1].path, "photos/tail.bmp", sizeof(fixtures[0].path));

    app_open_gallery(&ui);
    gallery_page_t *page = ui.page_context;
    assert(page->loading && !strcmp(request.path, "photos"));
    finish_catalog(&ui, GALLERY_CATALOG_LIMIT, GALLERY_CATALOG_LIMIT + 1);
    advance(50);
    assert(lv_obj_get_child_count(page->rows.viewport) == APP_SCROLL_LIST_ROWS + 1);
    assert(strstr(lv_label_get_text(page->summary), "4096 / 4097"));
    assert(catalog_queries == 1 && !task_creates);
    lv_obj_scroll_to_y(page->rows.viewport, GALLERY_CATALOG_LIMIT * page->rows.row_step, LV_ANIM_OFF);
    advance(50);
    const size_t tail = GALLERY_CATALOG_LIMIT - 1;
    assert(page->rows.indices[tail % APP_SCROLL_LIST_ROWS] == tail);
    tap(page->rows.rows[tail % APP_SCROLL_LIST_ROWS]);
    assert(page->selected == tail && page->job && task_creates == 1);
    run_worker();
    advance(80);
    assert(!page->job && page->pixels && page->descriptor.header.w == 2 && page->descriptor.header.h == 2);
    assert(!strcmp(lv_label_get_text(page->image_label), "tail.bmp"));
    assert(lv_obj_has_state(page->newer, LV_STATE_DISABLED));
    tap(page->older);
    assert(page->selected == tail - 2 && page->job && task_creates == 2);
    app_ui_begin_page(&ui, "桌面");
    run_worker();
    assert(!ui.page_context && !ui.timer);
    puts("PASS AC101S Gallery: 4096 rows reuse 12 buttons; real tail tap, BMP rendering, adjacent photo through directory, and cancellation");

    app_open_gallery(&ui);
    page = ui.page_context;
    finish_catalog(&ui, GALLERY_CATALOG_LIMIT, GALLERY_CATALOG_LIMIT);
    advance(50);
    const unsigned before_usb = catalog_queries;
    usb_blocked = true;
    ++generation;
    advance(300);
    assert(!page->rows.count && !page->ready && catalog_queries == before_usb);
    usb_blocked = false;
    advance(300);
    assert(page->loading && catalog_queries == before_usb + 1);
    finish_catalog(&ui, 10, 10);
    advance(50);
    tap(page->photos);
    assert(!strcmp(request.path, "photos/DCIM"));
    finish_catalog(&ui, 0, 0);
    tap(page->up);
    assert(!strcmp(request.path, "photos"));
    finish_catalog(&ui, 0, 0);
    for (unsigned i = 0; i < 30; ++i) {
        app_open_gallery(&ui);
        finish_catalog(&ui, 30, 30);
        advance(35);
        assert(lv_mem_test() == LV_RESULT_OK);
    }
    app_ui_begin_page(&ui, "桌面");
    lv_indev_delete(pointer);
    lv_display_delete(display);
    lv_deinit();
    puts("PASS AC101S Gallery: USB gate/remount, photos and parent navigation, and 30 repeated page rebuilds");
}
