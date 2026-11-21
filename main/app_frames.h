#pragma once

#include "esp_err.h"
#include "lvgl.h"

typedef struct app_frames *app_frames_handle_t;
typedef struct {
    uint32_t received_frames;
    uint32_t presented_frames;
    uint32_t replaced_frames;
    uint32_t queue_delay_us;
    uint32_t max_queue_delay_us;
} app_frames_metrics_t;
esp_err_t app_frames_new(app_frames_handle_t *out_frames);
/* Startup rollback only: callers must stop all producers and delete images first. */
void app_frames_delete(app_frames_handle_t frames);
/* May be called from a producer task. Never calls LVGL or waits for rendering. */
void app_frames_push(app_frames_handle_t frames, const uint16_t *pixels,
                      uint16_t width, uint16_t height);
/* Only the LVGL owner calls these functions. Buffers live for the UI lifetime. */
void app_frames_set_active(app_frames_handle_t frames, bool active);
/* GUI owner only. Fit the image object's dirty area to its actual aspect ratio
 * inside these bounds (max480x480), leaving letterbox pixels on the static
 * parent. Pass0,0 to retain the caller's existing image-object dimensions. */
void app_frames_set_viewport(app_frames_handle_t frames, uint16_t width, uint16_t height);
void app_frames_present(app_frames_handle_t frames, lv_obj_t *image);
/* Snapshot of the application-to-LVGL handover, not physical display FPS. */
esp_err_t app_frames_get_metrics(app_frames_handle_t frames, app_frames_metrics_t *metrics);
