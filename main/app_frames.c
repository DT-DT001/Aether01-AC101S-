#include "app_frames.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define FRAME_WIDTH 480U
#define FRAME_HEIGHT 480U
#define FRAME_BYTES (FRAME_WIDTH * FRAME_HEIGHT * sizeof(uint16_t))

struct app_frames {
    SemaphoreHandle_t lock;
    uint16_t *pixels[3];
    uint16_t width[3];
    uint16_t height[3];
    int64_t accepted_us[3];
    app_frames_metrics_t metrics;
    int front;
    int pending;
    bool active;
    uint16_t viewport_width, viewport_height;
    lv_image_dsc_t descriptor;
    lv_obj_t *refresh_image;
};

static void present_on_refresh(lv_event_t *event);
static void refresh_image_deleted(lv_event_t *event);

esp_err_t app_frames_new(app_frames_handle_t *out_frames)
{
    if (!out_frames) return ESP_ERR_INVALID_ARG;
    app_frames_handle_t frames = calloc(1, sizeof(*frames));
    if (!frames) return ESP_ERR_NO_MEM;
    frames->front = -1;
    frames->pending = -1;
    frames->lock = xSemaphoreCreateMutex();
    for (unsigned i = 0; i < 3; ++i) {
        frames->pixels[i] = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!frames->lock || !frames->pixels[0] || !frames->pixels[1] || !frames->pixels[2]) {
        for (unsigned i = 0; i < 3; ++i) free(frames->pixels[i]);
        if (frames->lock) vSemaphoreDelete(frames->lock);
        free(frames);
        return ESP_ERR_NO_MEM;
    }
    *out_frames = frames;
    return ESP_OK;
}

void app_frames_delete(app_frames_handle_t frames)
{
    if (!frames) return;
    app_frames_set_refresh_image(frames, NULL);
    for (unsigned i = 0; i < 3; ++i) free(frames->pixels[i]);
    vSemaphoreDelete(frames->lock);
    free(frames);
}

void app_frames_set_active(app_frames_handle_t frames, bool active)
{
    if (!frames) return;
    if (!active) app_frames_set_refresh_image(frames, NULL);
    /* Producer holds this only for a bounded PSRAM memcpy, never for I/O. */
    xSemaphoreTake(frames->lock, portMAX_DELAY);
    frames->active = active;
    if (active) memset(&frames->metrics, 0, sizeof(frames->metrics));
    frames->pending = -1;
    xSemaphoreGive(frames->lock);
}

void app_frames_set_refresh_image(app_frames_handle_t frames, lv_obj_t *image)
{
    if (!frames || frames->refresh_image == image) return;
    if (frames->refresh_image) {
        lv_display_remove_event_cb_with_user_data(lv_obj_get_display(frames->refresh_image),
                                                   present_on_refresh, frames);
        lv_obj_remove_event_cb_with_user_data(frames->refresh_image, refresh_image_deleted, frames);
    }
    frames->refresh_image = image;
    if (image) {
        lv_display_add_event_cb(lv_obj_get_display(image), present_on_refresh, LV_EVENT_REFR_START, frames);
        lv_obj_add_event_cb(image, refresh_image_deleted, LV_EVENT_DELETE, frames);
    }
}

static void refresh_image_deleted(lv_event_t *event)
{
    app_frames_handle_t frames = lv_event_get_user_data(event);
    if (frames->refresh_image == lv_event_get_target_obj(event))
        app_frames_set_refresh_image(frames, NULL);
}

void app_frames_set_viewport(app_frames_handle_t frames, uint16_t width, uint16_t height)
{
    if (!frames) return;
    xSemaphoreTake(frames->lock, portMAX_DELAY);
    frames->viewport_width = width && height ? (width > FRAME_WIDTH ? FRAME_WIDTH : width) : 0;
    frames->viewport_height = width && height ? (height > FRAME_HEIGHT ? FRAME_HEIGHT : height) : 0;
    xSemaphoreGive(frames->lock);
}

void app_frames_push(app_frames_handle_t frames, const uint16_t *pixels,
                      uint16_t width, uint16_t height)
{
    if (!frames || !pixels || !width || !height ||
        width > FRAME_WIDTH || height > FRAME_HEIGHT) return;
    if (xSemaphoreTake(frames->lock, 0) != pdTRUE) return;
    if (frames->active) {
        int slot = 0;
        while (slot == frames->front || slot == frames->pending) ++slot;
        memcpy(frames->pixels[slot], pixels, (size_t)width * height * sizeof(uint16_t));
        frames->width[slot] = width;
        frames->height[slot] = height;
        frames->accepted_us[slot] = esp_timer_get_time();
        ++frames->metrics.received_frames;
        if (frames->pending >= 0) ++frames->metrics.replaced_frames;
        frames->pending = slot; /* Replace an undisplayed frame instead of queuing latency. */
    }
    xSemaphoreGive(frames->lock);
}

static void present(app_frames_handle_t frames, lv_obj_t *image, bool refreshing)
{
    if (!frames || !image || xSemaphoreTake(frames->lock, 0) != pdTRUE) return;
    if (!refreshing && frames->refresh_image == image) {
        /* LVGL pauses its render timer when nothing is dirty. Request one
         * iteration; REFR_START will adopt the newest complete frame before
         * LVGL collects dirty areas, keeping it fixed through all TE/DMA strips. */
        if (frames->active && frames->pending >= 0) {
            lv_timer_t *timer = lv_display_get_refr_timer(lv_obj_get_display(image));
            if (timer) { lv_timer_resume(timer); lv_timer_ready(timer); }
        }
        xSemaphoreGive(frames->lock);
        return;
    }
    if (frames->active && frames->pending >= 0) {
        const int slot = frames->pending;
        frames->pending = -1;
        frames->front = slot;
        ++frames->metrics.presented_frames;
        const int64_t delay = esp_timer_get_time() - frames->accepted_us[slot];
        frames->metrics.queue_delay_us = delay <= 0 ? 0 : delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay;
        if (frames->metrics.queue_delay_us > frames->metrics.max_queue_delay_us)
            frames->metrics.max_queue_delay_us = frames->metrics.queue_delay_us;
        const bool same_source = refreshing && lv_image_get_src(image) == &frames->descriptor &&
            frames->descriptor.header.w == frames->width[slot] &&
            frames->descriptor.header.h == frames->height[slot];
        lv_image_cache_drop(&frames->descriptor);
        frames->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        frames->descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
        frames->descriptor.header.w = frames->width[slot];
        frames->descriptor.header.h = frames->height[slot];
        frames->descriptor.header.stride = frames->width[slot] * 2U;
        frames->descriptor.data_size = (uint32_t)frames->width[slot] * frames->height[slot] * 2U;
        frames->descriptor.data = (const uint8_t *)frames->pixels[slot];
        if (frames->viewport_width && frames->viewport_height) {
            /* The black letterbox belongs to the static parent. Restrict the
             * invalidated image object to the visible, proportional content. */
            uint32_t fitted_width = frames->viewport_width;
            uint32_t fitted_height = (uint32_t)frames->height[slot] * fitted_width / frames->width[slot];
            if (fitted_height > frames->viewport_height) {
                fitted_height = frames->viewport_height;
                fitted_width = (uint32_t)frames->width[slot] * fitted_height / frames->height[slot];
            }
            if (!fitted_width) fitted_width = 1;
            if (!fitted_height) fitted_height = 1;
            if (lv_obj_get_width(image) != (int32_t)fitted_width ||
                lv_obj_get_height(image) != (int32_t)fitted_height) {
                lv_obj_set_size(image, (int32_t)fitted_width, (int32_t)fitted_height);
                lv_obj_center(image);
            }
        }
        if (!same_source) lv_image_set_src(image, &frames->descriptor);
        if (!frames->viewport_width || !frames->viewport_height) lv_obj_center(image);
        lv_obj_invalidate(image);
    }
    xSemaphoreGive(frames->lock);
}

void app_frames_present(app_frames_handle_t frames, lv_obj_t *image)
{
    present(frames, image, false);
}

static void present_on_refresh(lv_event_t *event)
{
    app_frames_handle_t frames = lv_event_get_user_data(event);
    present(frames, frames->refresh_image, true);
}

esp_err_t app_frames_get_metrics(app_frames_handle_t frames, app_frames_metrics_t *metrics)
{
    if (!frames || !metrics) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(frames->lock, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    *metrics = frames->metrics;
    xSemaphoreGive(frames->lock);
    return ESP_OK;
}
