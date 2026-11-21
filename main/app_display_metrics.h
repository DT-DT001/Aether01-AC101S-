#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "lvgl.h"

typedef struct {
    bool available;
    bool active;
    uint32_t completed_refreshes;
    uint32_t completed_regions;
    uint32_t refresh_fps_x100;
    uint32_t max_refresh_gap_us;
    /* Lifetime DMA wait diagnostics; unlike media FPS these do not reset when
     * measurement is paused. A timeout reports a stall, never buffer release. */
    bool flush_wait_available;
    bool flush_waiting;
    bool flush_stalled;
    uint64_t submitted_sequence;
    uint64_t completed_sequence;
    uint32_t flush_wait_calls;
    uint32_t flush_wait_timeouts;
    uint32_t max_flush_wait_us;
    bool staging_available;
    size_t staging_capacity;
    uint64_t staging_copied_bytes;
    uint32_t dropped_regions;
    esp_err_t staging_error;
} app_display_metrics_t;

/* SPI/QSPI, project-pinned LVGL 9.5 only. Call while holding the recursive
 * LVGL lock, before the first render and with no in-flight color transfer.
 * Adds a blocking DMA wait without changing the port's flush/ready callbacks'
 * buffer ownership. On failure the original LVGL/port hooks remain installed.
 * Display/IO and this lifetime semaphore must remain alive for the application. */
esp_err_t app_display_metrics_attach(esp_lcd_panel_io_handle_t io, lv_display_t *display);
/* Same lifetime/locking contract, with a fixed 64-byte-aligned INTERNAL|DMA
 * mirror for packed partial RGB565. The original port swaps mirror bytes;
 * LVGL may render in PSRAM without direct PSRAM DMA. Capacity is bytes, not
 * a height limit: narrow/tall chunks are valid when their packed data fits.
 * Allocation/registration failure leaves original hooks unchanged; callers
 * using PSRAM buffers must treat failure as fatal rather than direct-flush. */
esp_err_t app_display_metrics_attach_staged_rgb565(esp_lcd_panel_io_handle_t io, lv_display_t *display, size_t capacity);
void app_display_metrics_set_active(bool active);
void app_display_metrics_get(app_display_metrics_t *metrics);
