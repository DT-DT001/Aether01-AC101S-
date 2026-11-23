#include "app_display_metrics.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
/* Project-pinned LVGL 9.5: there is no public flush-callback getter. Read
 * the existing callback once under the caller's LVGL lock, then install our
 * allocation-free wrapper through the public setter. No managed code changes. */
#include "src/display/lv_display_private.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_display_metrics_t s_metrics;
static lv_display_t *s_display;
static int64_t s_window_start_us;
static int64_t s_previous_us;
static uint32_t s_window_frames;
#if AETHER_UI_SELFTEST
static bool s_probe_active;
#endif
static struct {
    SemaphoreHandle_t signal;
    lv_display_flush_cb_t original;
    esp_lcd_panel_handle_t panel;
    uint64_t submitted, completed;
    uint32_t calls, timeouts, max_wait_us;
    bool waiting, stalled;
    uint8_t *mirror;
    size_t capacity;
    uint64_t copied_bytes;
    uint32_t dropped_regions;
    esp_err_t staging_error;
} s_wait;

static void wait_for_dma(lv_display_t *display);

static bool packed_rgb565_region(lv_display_t *display, const lv_area_t *area,
                                const uint8_t *pixels, size_t *bytes)
{
    if (!area || !pixels || lv_display_get_render_mode(display) != LV_DISPLAY_RENDER_MODE_PARTIAL ||
        lv_display_get_color_format(display) != LV_COLOR_FORMAT_RGB565) return false;
    const int32_t hres = lv_display_get_horizontal_resolution(display);
    const int32_t vres = lv_display_get_vertical_resolution(display);
    if (hres <= 0 || vres <= 0 || area->x1 < 0 || area->y1 < 0 ||
        area->x2 < area->x1 || area->y2 < area->y1 || area->x2 >= hres || area->y2 >= vres ||
        (area->x1 & 1) || (area->y1 & 1) || !(area->x2 & 1) || !(area->y2 & 1)) return false;
    const size_t width = (size_t)area->x2 - (size_t)area->x1 + 1;
    const size_t height = (size_t)area->y2 - (size_t)area->y1 + 1;
    if (width > SIZE_MAX / 2) return false;
    const size_t row_bytes = width * 2;
    if (height > s_wait.capacity / row_bytes) return false;
    const size_t length = row_bytes * height;
    lv_draw_buf_t *buffer = lv_display_get_buf_active(display);
    if (!buffer || !buffer->data || buffer->header.cf != LV_COLOR_FORMAT_RGB565 ||
        buffer->header.w != width || buffer->header.h < height || buffer->header.stride != row_bytes) return false;
    const uintptr_t begin = (uintptr_t)buffer->data, pointer = (uintptr_t)pixels;
    if (pointer < begin || pointer - begin > buffer->data_size ||
        length > buffer->data_size - (pointer - begin)) return false;
    *bytes = length;
    return true;
}

static void copy_rgb565(uint8_t *dest, const uint8_t *source, size_t bytes)
{
    /* Even width and height make every packed region a multiple of 4 bytes.
     * memcpy keeps this valid for any LVGL source alignment and aliases. */
    for (size_t offset = 0; offset < bytes; offset += sizeof(uint32_t)) {
        uint32_t pair;
        memcpy(&pair, source + offset, sizeof(pair));
        pair = ((pair & UINT32_C(0x00FF00FF)) << 8) |
               ((pair & UINT32_C(0xFF00FF00)) >> 8);
        memcpy(dest + offset, &pair, sizeof(pair));
    }
}

static void submit_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    if (!s_wait.panel) {
        portENTER_CRITICAL(&s_lock);
        ++s_wait.submitted;
        portEXIT_CRITICAL(&s_lock);
        s_wait.original(display, area, pixels);
        return;
    }

    /* Flush-ready can expose the next buffer before the previous ISR
     * publishes completion. The mirror cannot be reused during that gap. */
    portENTER_CRITICAL(&s_lock);
    const bool pending = s_wait.submitted != s_wait.completed;
    portEXIT_CRITICAL(&s_lock);
    if (pending) wait_for_dma(display);
    size_t bytes;
    if (!packed_rgb565_region(display, area, pixels, &bytes)) {
        portENTER_CRITICAL(&s_lock);
        ++s_wait.dropped_regions;
        s_wait.staging_error = ESP_ERR_INVALID_ARG;
        portEXIT_CRITICAL(&s_lock);
        lvgl_port_flush_ready(display);
        return;
    }
    copy_rgb565(s_wait.mirror, pixels, bytes);
    portENTER_CRITICAL(&s_lock);
    s_wait.copied_bytes += bytes;
    s_wait.staging_error = ESP_OK;
    ++s_wait.submitted;
    portEXIT_CRITICAL(&s_lock);
    const esp_err_t error = esp_lcd_panel_draw_bitmap(s_wait.panel, area->x1, area->y1,
        area->x2 + 1, area->y2 + 1, s_wait.mirror);
    if (error != ESP_OK) {
        /* BSP max_transfer_bytes is at least one mirror capacity, so the
         * driver queues a single color transaction or returns without one. */
        portENTER_CRITICAL(&s_lock);
        --s_wait.submitted;
        ++s_wait.dropped_regions;
        s_wait.staging_error = error;
        portEXIT_CRITICAL(&s_lock);
        lvgl_port_flush_ready(display);
    }
}

static void wait_for_dma(lv_display_t *display)
{
    (void)display;
    const int64_t began = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    const uint64_t target = s_wait.submitted;
    SemaphoreHandle_t signal = s_wait.signal;
    ++s_wait.calls;
    portEXIT_CRITICAL(&s_lock);
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        const bool done = s_wait.completed >= target;
        s_wait.waiting = !done;
        if (done) {
            s_wait.stalled = false;
            const int64_t elapsed = esp_timer_get_time() - began;
            const uint32_t bounded = elapsed > UINT32_MAX ? UINT32_MAX : elapsed > 0 ? (uint32_t)elapsed : 0;
            if (bounded > s_wait.max_wait_us) s_wait.max_wait_us = bounded;
        }
        portEXIT_CRITICAL(&s_lock);
        if (done) return;
        /* A semaphore is only a wake hint. A previous callback may publish
         * its token after flush-ready let the next chunk start on another
         * core. Recheck this chunk's sequence after every wake and timeout.
         * LVGL clears `flushing` as soon as this callback returns: a timeout
         * must NEVER return and release a buffer still owned by DMA. */
        TickType_t interval = pdMS_TO_TICKS(100);
        if (!interval) interval = 1;
        if (xSemaphoreTake(signal, interval) != pdTRUE) {
            portENTER_CRITICAL(&s_lock);
            if (s_wait.completed < target) { ++s_wait.timeouts; s_wait.stalled = true; }
            portEXIT_CRITICAL(&s_lock);
        }
    }
}

static bool color_done(esp_lcd_panel_io_handle_t io,
                       esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)io;
    (void)event;
    lv_display_t *display = context;
    const bool last = lv_display_flush_is_last(display);
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_lock);
    if (s_metrics.active) {
        ++s_metrics.completed_regions;
        if (last) {
            ++s_metrics.completed_refreshes;
            ++s_window_frames;
            if (s_previous_us && now >= s_previous_us) {
                const int64_t gap = now - s_previous_us;
                const uint32_t bounded = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
                if (bounded > s_metrics.max_refresh_gap_us) s_metrics.max_refresh_gap_us = bounded;
            }
            s_previous_us = now;
            const int64_t elapsed = now - s_window_start_us;
            if (elapsed >= 1000000) {
                s_metrics.refresh_fps_x100 = (uint32_t)((uint64_t)s_window_frames * 100000000ULL / (uint64_t)elapsed);
                s_window_frames = 0;
                s_window_start_us = now;
            }
        }
    }
    portEXIT_CRITICAL_ISR(&s_lock);
    /* Equivalent to esp_lvgl_port's installed SPI completion callback. */
    lvgl_port_flush_ready(display);
    /* Publish AFTER flush-ready: publishing beforehand would let the waiter
     * start another flush whose flag this old callback could then clear. */
    portENTER_CRITICAL_ISR(&s_lock);
    ++s_wait.completed;
    SemaphoreHandle_t signal = s_wait.signal;
    portEXIT_CRITICAL_ISR(&s_lock);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(signal, &woken);
    /* This installed IDF's SPI panel IO ignores the callback's bool return.
     * Request the yield explicitly instead of waiting for the next tick. */
    if (woken == pdTRUE) portYIELD_FROM_ISR();
    return woken == pdTRUE;
}

static esp_err_t attach(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel,
                        lv_display_t *display, size_t capacity)
{
    if (!io || !display) return ESP_ERR_INVALID_ARG;
    /* Attach before the first refresh while holding the recursive LVGL lock.
     * One lifetime display is supported; do not replace an existing wait hook. */
    if (s_display || display->flushing || !display->flush_cb || display->flush_wait_cb) return ESP_ERR_INVALID_STATE;
    uint8_t *mirror = NULL;
    if (capacity) {
        if (lv_display_get_render_mode(display) != LV_DISPLAY_RENDER_MODE_PARTIAL ||
            lv_display_get_color_format(display) != LV_COLOR_FORMAT_RGB565) return ESP_ERR_NOT_SUPPORTED;
        if (capacity < 8 || capacity > SIZE_MAX - 63) return ESP_ERR_INVALID_ARG;
        mirror = heap_caps_aligned_alloc(64, (capacity + 63) & ~(size_t)63,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!mirror) return ESP_ERR_NO_MEM;
    }
    SemaphoreHandle_t signal = xSemaphoreCreateBinary();
    if (!signal) { free(mirror); return ESP_ERR_NO_MEM; }
    const esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = color_done};
    const esp_err_t error = esp_lcd_panel_io_register_event_callbacks(io, &callbacks, display);
    if (error == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_wait.signal = signal;
        s_wait.original = display->flush_cb;
        s_wait.panel = panel;
        s_wait.mirror = mirror;
        s_wait.capacity = capacity;
        s_display = display;
        s_metrics = (app_display_metrics_t){.available = true};
        s_previous_us = 0;
        s_window_frames = 0;
        s_window_start_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_lock);
        lv_display_set_flush_cb(display, submit_flush);
        lv_display_set_flush_wait_cb(display, wait_for_dma);
    } else {
        /* Neither LVGL hook has changed: the port's original busy-wait path
         * and completion callback remain usable on registration/OOM failure. */
        vSemaphoreDelete(signal);
        free(mirror);
    }
    return error;
}

esp_err_t app_display_metrics_attach(esp_lcd_panel_io_handle_t io, lv_display_t *display)
{ return attach(io, NULL, display, 0); }

esp_err_t app_display_metrics_attach_staged_rgb565(esp_lcd_panel_io_handle_t io,
    esp_lcd_panel_handle_t panel, lv_display_t *display, size_t capacity)
{ return panel && capacity ? attach(io, panel, display, capacity) : ESP_ERR_INVALID_ARG; }

static void set_active_locked(bool active)
{
    if (s_metrics.active != active) {
        const bool available = s_display != NULL && s_metrics.available;
        s_metrics = (app_display_metrics_t){.available = available, .active = active};
        s_previous_us = 0;
        s_window_frames = 0;
        s_window_start_us = esp_timer_get_time();
    }
}

void app_display_metrics_set_active(bool active)
{
    portENTER_CRITICAL(&s_lock);
#if AETHER_UI_SELFTEST
    active |= s_probe_active;
#endif
    set_active_locked(active);
    portEXIT_CRITICAL(&s_lock);
}

#if AETHER_UI_SELFTEST
void app_display_metrics_set_probe_active(bool active)
{
    portENTER_CRITICAL(&s_lock);
    s_probe_active = active;
    set_active_locked(active);
    portEXIT_CRITICAL(&s_lock);
}
#endif

void app_display_metrics_get(app_display_metrics_t *metrics)
{
    if (!metrics) return;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    *metrics = s_metrics;
    metrics->flush_wait_available = s_wait.signal != NULL;
    metrics->flush_waiting = s_wait.waiting;
    metrics->flush_stalled = s_wait.stalled;
    metrics->submitted_sequence = s_wait.submitted;
    metrics->completed_sequence = s_wait.completed;
    metrics->flush_wait_calls = s_wait.calls;
    metrics->flush_wait_timeouts = s_wait.timeouts;
    metrics->max_flush_wait_us = s_wait.max_wait_us;
    metrics->staging_available = s_wait.mirror != NULL;
    metrics->staging_capacity = s_wait.capacity;
    metrics->staging_copied_bytes = s_wait.copied_bytes;
    metrics->dropped_regions = s_wait.dropped_regions;
    metrics->staging_error = s_wait.staging_error;
    if (!s_previous_us || now - s_previous_us >= 1500000) metrics->refresh_fps_x100 = 0;
    portEXIT_CRITICAL(&s_lock);
}
