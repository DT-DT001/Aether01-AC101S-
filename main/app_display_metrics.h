#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "lvgl.h"

typedef struct {
    uint32_t timed_frames;
    uint32_t late_frames;
    uint32_t max_te_to_submit_us;
    uint32_t max_te_to_done_us;
    int32_t min_scan_margin_us;
    uint32_t direct_frames;
    uint32_t direct_timed_frames;
    uint32_t direct_late_frames;
    int32_t direct_min_scan_margin_us;
    uint32_t direct_max_copy_us;
    uint32_t direct_max_submit_us;
    uint32_t direct_max_wait_us;
} app_display_nes_metrics_t;

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
    app_display_nes_metrics_t nes;
} app_display_metrics_t;

/* SPI/QSPI, project-pinned LVGL 9.5 only. Call while holding the recursive
 * LVGL lock, before the first render and with no in-flight color transfer.
 * Adds a blocking DMA wait without changing the port's flush/ready callbacks'
 * buffer ownership. On failure the original LVGL/port hooks remain installed.
 * Display/IO and this lifetime semaphore must remain alive for the application. */
esp_err_t app_display_metrics_attach(esp_lcd_panel_io_handle_t io, lv_display_t *display);
/* Same lifetime/locking contract, with a fixed 64-byte-aligned INTERNAL|DMA
 * mirror for packed partial RGB565, with byte order converted during copying.
 * LVGL may render in PSRAM without direct PSRAM DMA. Capacity is bytes, not
 * a height limit: narrow/tall chunks are valid when their packed data fits.
 * Allocation/registration failure leaves original hooks unchanged; callers
 * using PSRAM buffers must treat failure as fatal rather than direct-flush. */
esp_err_t app_display_metrics_attach_staged_rgb565(esp_lcd_panel_io_handle_t io,
    esp_lcd_panel_handle_t panel, lv_display_t *display, size_t capacity);
void app_display_metrics_set_active(bool active);
#if AETHER_UI_SELFTEST
/* Temporarily measure non-video pages without the media poll disabling it. */
void app_display_metrics_set_probe_active(bool active);
#endif
void app_display_metrics_get(app_display_metrics_t *metrics);

/* LVGL owner only. Track the steady-state NES viewport only when consecutive
 * flushes cover this exact rectangle after the same TE. Evaluate every strip
 * at DMA completion against its last row's estimated scan time. */
void app_display_metrics_nes_reset(void);
void app_display_metrics_nes_arm(const lv_area_t *area, int64_t edge_us,
                                 uint32_t period_us, uint16_t panel_rows);
/* GUI owner only. Prepare the first strip before waiting for TE. The caller
 * owns the complete packed RGB565 source until this function returns. Only
 * the permanent staging mirror remains reserved if the DMA stalls. */
typedef esp_err_t (*app_display_nes_wait_te_fn)(void *ctx);
typedef esp_err_t (*app_display_nes_strip_fn)(void *ctx, const lv_area_t *first_window,
                                             const void *pixels, size_t bytes);
esp_err_t app_display_metrics_nes_write(lv_display_t *display, const lv_area_t *area,
    const uint16_t *pixels, app_display_nes_wait_te_fn wait_te,
    app_display_nes_strip_fn write_strip, void *ctx);
