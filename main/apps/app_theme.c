#include "app_ui_internal.h"
#include "app_theme.h"
#include "app_wallpaper_store.h"
#include "app_catalog_ui.h"

#include <stdlib.h>
#include "freertos/task.h"
#include "nvs.h"
#include "src/misc/cache/instance/lv_image_cache.h"

static const struct {
    uint32_t accent, background;
    const char *name;
} palettes[] = {
    {0x9EC9FF, 0x162537, "经典蓝"}, {0x80E1CC, 0x122C29, "青绿"},
    {0xFFBA87, 0x39271D, "夕阳橙"}, {0xCBB5FF, 0x2B2243, "紫罗兰"},
    {0xFFB6D5, 0x352237, "粉红"}, {0xC6D6E4, 0x292F36, "石墨灰"},
};
enum { PALETTE_COUNT = sizeof(palettes) / sizeof(palettes[0]) };

typedef struct {
    lv_image_dsc_t image;
    unsigned refs;
} wallpaper_t;

static app_wallpaper_store_t wallpaper_store;
static wallpaper_t *wallpaper;
static uint8_t palette;

/* Only the UI allocates/installs previews. The worker does not use LVGL. */
static struct {
    bool running, change_palette;
    atomic_bool done;
    uint8_t palette;
    wallpaper_t *wallpaper;
    esp_err_t result;
} write_job;

static wallpaper_t *wallpaper_new(uint16_t *pixels, uint16_t width, uint16_t height)
{
    wallpaper_t *value = calloc(1, sizeof(*value));
    if (!value) return NULL;
    value->refs = 1;
    value->image = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                   .w = width, .h = height, .stride = width * sizeof(uint16_t)},
        .data_size = (uint32_t)width * height * sizeof(uint16_t),
        .data = (const uint8_t *)pixels,
    };
    return value;
}

static void wallpaper_release(wallpaper_t *value)
{
    if (value && --value->refs == 0) {
        lv_image_cache_drop(&value->image);
        free((void *)value->image.data);
        free(value);
    }
}

void app_theme_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open("appearance", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t saved;
        if (nvs_get_u8(nvs, "palette", &saved) == ESP_OK && saved < PALETTE_COUNT) palette = saved;
        nvs_close(nvs);
    }
    uint16_t *pixels, width, height;
    if (app_wallpaper_store_open(&wallpaper_store, &pixels, &width, &height) == ESP_OK && pixels) {
        wallpaper = wallpaper_new(pixels, width, height);
        if (!wallpaper) free(pixels);
    }
}

static void theme_write_task(void *arg)
{
    (void)arg;
    if (write_job.change_palette) {
        nvs_handle_t nvs;
        esp_err_t error = nvs_open("appearance", NVS_READWRITE, &nvs);
        if (error == ESP_OK) {
            error = nvs_set_u8(nvs, "palette", write_job.palette);
            if (error == ESP_OK) error = nvs_commit(nvs);
            nvs_close(nvs);
        }
        write_job.result = error;
    } else {
        const lv_image_dsc_t *image = write_job.wallpaper ? &write_job.wallpaper->image : NULL;
        write_job.result = app_wallpaper_store_write(&wallpaper_store,
            image ? (const uint16_t *)image->data : NULL, image ? image->header.w : 0,
            image ? image->header.h : 0);
    }
    atomic_store_explicit(&write_job.done, true, memory_order_release);
    vTaskDelete(NULL);
}

esp_err_t app_theme_write_status(void)
{
    if (!write_job.running) return write_job.result;
    if (!atomic_load_explicit(&write_job.done, memory_order_acquire)) return ESP_ERR_NOT_FINISHED;
    if (write_job.change_palette) {
        if (write_job.result == ESP_OK) palette = write_job.palette;
    } else if (write_job.result == ESP_OK) {
        wallpaper_t *old = wallpaper;
        wallpaper = write_job.wallpaper;
        wallpaper_release(old);
    } else {
        wallpaper_release(write_job.wallpaper);
    }
    write_job.wallpaper = NULL;
    write_job.running = false;
    return write_job.result;
}

bool app_theme_wallpaper_available(void)
{
    /* The partition pointer is set during initialization and then immutable. */
    return wallpaper_store.partition != NULL;
}

static esp_err_t start_write(bool change_palette, uint8_t index, wallpaper_t *value)
{
    write_job.running = true;
    write_job.change_palette = change_palette;
    write_job.palette = index;
    write_job.wallpaper = value;
    atomic_store(&write_job.done, false);
    if (xTaskCreate(theme_write_task, "theme_save", 4096, NULL, 3, NULL) == pdPASS) return ESP_OK;
    write_job.wallpaper = NULL;
    write_job.running = false;
    return ESP_ERR_NO_MEM;
}

esp_err_t app_theme_write_wallpaper(uint16_t *pixels, uint16_t width, uint16_t height)
{
    if (!app_theme_wallpaper_available() || app_theme_write_status() == ESP_ERR_NOT_FINISHED)
        return ESP_ERR_INVALID_STATE;
    if (pixels ? !width || !height || width > APP_WALLPAPER_SIZE || height > APP_WALLPAPER_SIZE : width || height)
        return ESP_ERR_INVALID_ARG;
    wallpaper_t *value = pixels ? wallpaper_new(pixels, width, height) : NULL;
    if (pixels && !value) return ESP_ERR_NO_MEM;
    const esp_err_t error = start_write(false, 0, value);
    if (error != ESP_OK) free(value); /* The caller retains pixels on submission failure. */
    return error;
}

static uint32_t mix_color(uint32_t base, uint32_t tint)
{
    uint32_t mixed = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const uint32_t sample = (((base >> shift) & 255U) + ((tint >> shift) & 255U) + 1U) / 2U;
        mixed |= sample << shift;
    }
    return mixed;
}

uint32_t app_theme_color(uint32_t fallback)
{
    return palette ? mix_color(fallback, palettes[palette].accent) : fallback;
}

uint32_t app_theme_background(uint32_t fallback)
{
    return palette ? mix_color(fallback, palettes[palette].background) : fallback;
}

const lv_image_dsc_t *app_theme_wallpaper(void)
{
    (void)app_theme_write_status();
    return wallpaper ? &wallpaper->image : NULL;
}

static void wallpaper_view_deleted(lv_event_t *event)
{
    wallpaper_release(lv_event_get_user_data(event));
}

lv_obj_t *app_theme_wallpaper_view(lv_obj_t *parent)
{
    const lv_image_dsc_t *source = app_theme_wallpaper();
    if (!source) return NULL;
    lv_obj_t *image = lv_image_create(parent);
    if (!image) return NULL;
    ++wallpaper->refs;
    lv_obj_add_event_cb(image, wallpaper_view_deleted, LV_EVENT_DELETE, wallpaper);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_image_set_src(image, source);
    lv_image_set_pivot(image, source->header.w / 2, source->header.h / 2);
    lv_obj_center(image);
    return image;
}

typedef struct {
    lv_obj_t *status, *pick, *clear, *swatches[PALETTE_COUNT];
    void (*pending_exit)(app_ui_t *);
    bool saving;
} theme_page_t;

static void theme_cleanup(app_ui_t *ui)
{
    free(ui->page_context);
    ui->page_context = NULL;
}

static bool theme_can_exit(app_ui_t *ui, void (*destination)(app_ui_t *))
{
    theme_page_t *page = ui->page_context;
    if (!page->saving) return true;
    page->pending_exit = destination;
    return false;
}

static void theme_controls(theme_page_t *page)
{
    const bool available = app_theme_wallpaper_available();
    lv_obj_set_state(page->pick, LV_STATE_DISABLED, page->saving || !available);
    lv_obj_set_state(page->clear, LV_STATE_DISABLED, page->saving || !available || !wallpaper);
    for (unsigned i = 0; i < PALETTE_COUNT; ++i)
        lv_obj_set_state(page->swatches[i], LV_STATE_DISABLED, page->saving);
}

static void theme_timer(lv_timer_t *timer)
{
    app_ui_t *ui = lv_timer_get_user_data(timer);
    theme_page_t *page = ui->page_context;
    const esp_err_t error = app_theme_write_status();
    if (error == ESP_ERR_NOT_FINISHED) return;
    page->saving = false;
    if (page->pending_exit) { page->pending_exit(ui); return; }
    if (error == ESP_OK) { app_open_theme(ui); return; }
    app_ui_clear_timer(ui);
    app_ui_label_text(page->status, "保存失败，请重试");
    theme_controls(page);
}

static void start_wait(app_ui_t *ui, esp_err_t error)
{
    theme_page_t *page = ui->page_context;
    if (error != ESP_OK) { app_ui_label_text(page->status, "无法保存，请重试"); return; }
    page->saving = true;
    app_ui_label_text(page->status, "正在保存…");
    theme_controls(page);
    app_ui_set_timer(ui, theme_timer, 40);
}

static void pick_color(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    theme_page_t *page = ui->page_context;
    const unsigned index = (uintptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(event));
    if (!page || page->saving || index == palette || index >= PALETTE_COUNT ||
        app_theme_write_status() == ESP_ERR_NOT_FINISHED) return;
    start_wait(ui, start_write(true, index, NULL));
}

static void pick_photo(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    theme_page_t *page = ui->page_context;
    if (page && !page->saving && app_theme_wallpaper_available()) app_open_wallpaper_picker(ui);
}

static void clear_wallpaper(lv_event_t *event)
{
    app_ui_t *ui = lv_event_get_user_data(event);
    theme_page_t *page = ui->page_context;
    if (page && !page->saving && wallpaper) start_wait(ui, app_theme_write_wallpaper(NULL, 0, 0));
}

static lv_obj_t *theme_button(lv_obj_t *parent, const char *text, int x, int width,
                             lv_event_cb_t handler, app_ui_t *ui)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, 307);
    lv_obj_set_size(button, width, 44);
    app_catalog_control(button, app_theme_background(0x26384F));
    lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED, ui);
    app_ui_add_text(button, text, LV_ALIGN_CENTER, 0, 0, app_theme_color(0xDBEDFF), NULL);
    return button;
}

static void theme_preview(lv_obj_t *parent)
{
    lv_obj_t *preview = lv_obj_create(parent);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(preview, 408, 155);
    lv_obj_set_style_pad_all(preview, 0, 0);
    lv_obj_set_style_radius(preview, 21, 0);
    lv_obj_set_style_clip_corner(preview, true, 0);
    lv_obj_set_style_border_width(preview, 1, 0);
    lv_obj_set_style_border_color(preview, lv_color_hex(app_theme_background(0x485574)), 0);
    lv_obj_set_style_bg_color(preview, lv_color_hex(app_theme_background(0x17273C)), 0);
    lv_obj_set_style_bg_grad_color(preview, lv_color_hex(app_theme_background(0x223851)), 0);
    lv_obj_set_style_bg_grad_dir(preview, LV_GRAD_DIR_VER, 0);
    lv_obj_t *image = app_theme_wallpaper_view(preview);
    if (image) {
        const lv_image_dsc_t *source = &wallpaper->image;
        const uint32_t scale = LV_MIN(404U * LV_SCALE_NONE / source->header.w,
                                      151U * LV_SCALE_NONE / source->header.h);
        lv_image_set_scale(image, LV_MIN(LV_SCALE_NONE, scale));
        lv_obj_set_style_image_opa(image, LV_OPA_80, 0);
    }
    app_ui_add_text(preview, "桌面预览", LV_ALIGN_TOP_LEFT, 16, 12, 0xFFFFFF, NULL);
    app_ui_add_text(preview, "10:24", LV_ALIGN_CENTER, 0, 2, 0xFFFFFF, &lv_font_montserrat_48);
}

void app_open_theme(app_ui_t *ui)
{
    (void)app_theme_write_status();
    lv_obj_t *content = app_ui_begin_page(ui, "主题");
    theme_page_t *page = calloc(1, sizeof(*page));
    if (!page) { app_ui_add_text(content, "内存不足", LV_ALIGN_CENTER, 0, 0, 0xFF8888, NULL); return; }
    ui->page_context = page;
    ui->page_cleanup = theme_cleanup;
    ui->page_can_exit = theme_can_exit;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    theme_preview(content);

    app_ui_add_text(content, "系统配色", LV_ALIGN_TOP_LEFT, 0, 172, 0xF1F6FF, app_ui_chinese_font_size(24));
    app_ui_add_text(content, palettes[palette].name, LV_ALIGN_TOP_RIGHT, -2, 178,
                    palettes[palette].accent, NULL);
    for (unsigned i = 0; i < PALETTE_COUNT; ++i) {
        lv_obj_t *button = page->swatches[i] = lv_button_create(content);
        lv_obj_set_pos(button, 5 + i * 68, 210);
        lv_obj_set_size(button, 56, 48);
        app_catalog_control(button, palettes[i].accent);
        lv_obj_set_style_bg_color(button, lv_color_hex(palettes[i].accent), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(button, lv_color_hex(palettes[i].accent), LV_STATE_CHECKED);
        lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), LV_STATE_CHECKED);
        lv_obj_set_style_border_width(button, 3, LV_STATE_CHECKED);
        lv_obj_set_user_data(button, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(button, pick_color, LV_EVENT_CLICKED, ui);
        if (i == palette) {
            lv_obj_add_state(button, LV_STATE_CHECKED);
            app_ui_add_text(button, LV_SYMBOL_OK, LV_ALIGN_CENTER, 0, 0, 0x102338, &lv_font_montserrat_20);
        }
    }
    const char *status = !app_theme_wallpaper_available() ? "壁纸存储不可用" : wallpaper ?
        "壁纸已保存到设备" : "从 TF 卡导入 JPEG 或 BMP";
    page->status = app_ui_add_text(content, status, LV_ALIGN_TOP_MID, 0, 276,
                                  app_theme_color(0xBACBE0), NULL);
    page->pick = theme_button(content, "从 TF 卡选择", 0, 237, pick_photo, ui);
    page->clear = theme_button(content, "清除壁纸", 247, 161, clear_wallpaper, ui);
    theme_controls(page);
}
