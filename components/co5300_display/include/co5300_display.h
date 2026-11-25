#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t host;
    gpio_num_t clk;
    gpio_num_t data0;
    gpio_num_t data1;
    gpio_num_t data2;
    gpio_num_t data3;
    gpio_num_t cs;
    gpio_num_t reset;
    gpio_num_t te;
    uint32_t clock_hz;
    uint16_t width;
    uint16_t height;
    size_t max_transfer_bytes;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_done;
    void *user_ctx;
} co5300_display_config_t;

esp_err_t co5300_display_new(const co5300_display_config_t *config,
                             esp_lcd_panel_io_handle_t *out_io,
                             esp_lcd_panel_handle_t *out_panel);

/** Ordinary QSPI DCS sleep with the panel supplies retained. Serialize against
 * LVGL drawing. Never enters vendor 0x4F deep standby; settles for 120ms. */
esp_err_t co5300_display_set_sleep(esp_lcd_panel_io_handle_t io, bool sleeping);

typedef struct {
    uint16_t x_start, y_start, x_end, y_end; /* Exclusive ends. */
} co5300_window_t;

/* Serialize against every panel command for the full stream. Supply an
 * even-aligned exact window on the first strip, then NULL to continue it.
 * The caller holds the RGB565 wire-order strip immutable until color-done.
 * A strip fits one bus transaction and generates one color-done callback. */
esp_err_t co5300_display_write_rgb565_strip(esp_lcd_panel_io_handle_t io,
    const co5300_window_t *first_window, const void *pixels, size_t bytes);

typedef struct {
    bool available;
    gpio_num_t pin;
    esp_err_t setup_error;
    uint32_t edges, waits, synced, timeouts, cancelled, fallback_frames;
    uint32_t last_wait_us, max_wait_us, last_period_us;
    int64_t last_edge_us;
} co5300_te_metrics_t;
/** Wait for a new rising TE edge in task context. timeout_ms must be <=100.
 * TIMEOUT/NOT_SUPPORTED permit unsynchronized fallback; never call from ISR.
 * Only one waiter is allowed. Missing TE retries use a bounded cooldown. */
esp_err_t co5300_display_wait_te(esp_lcd_panel_io_handle_t io, uint32_t timeout_ms);
/** Cancel a current wait, e.g. before sleep/reset. Does not disable future TE. */
void co5300_display_cancel_te(esp_lcd_panel_io_handle_t io);
esp_err_t co5300_display_get_te_metrics(esp_lcd_panel_io_handle_t io, co5300_te_metrics_t *metrics);

#ifdef __cplusplus
}
#endif
