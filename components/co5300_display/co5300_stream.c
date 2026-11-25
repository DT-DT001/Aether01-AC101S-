#include "co5300_display.h"

#define CO5300_QSPI_COMMAND(command) ((0x02U << 24) | ((command) << 8))
#define CO5300_QSPI_COLOR(command) ((0x32U << 24) | ((command) << 8))

esp_err_t co5300_display_write_rgb565_strip(esp_lcd_panel_io_handle_t io,
    const co5300_window_t *first_window, const void *pixels, size_t bytes)
{
    if (!io || !pixels || !bytes || (bytes & 3U)) return ESP_ERR_INVALID_ARG;
    if (!first_window)
        return esp_lcd_panel_io_tx_color(io, CO5300_QSPI_COLOR(0x3CU), pixels, bytes);

    const co5300_window_t w = *first_window;
    if (w.x_start >= w.x_end || w.y_start >= w.y_end ||
        w.x_end > 480 || w.y_end > 480 ||
        ((w.x_start | w.y_start | w.x_end | w.y_end) & 1U))
        return ESP_ERR_INVALID_ARG;
    const size_t row_bytes = 2U * (w.x_end - w.x_start);
    if (bytes % (row_bytes * 2U) || bytes > row_bytes * (w.y_end - w.y_start))
        return ESP_ERR_INVALID_ARG;

    const uint16_t x_last = w.x_end - 1, y_last = w.y_end - 1;
    const uint8_t columns[] = {w.x_start >> 8, w.x_start, x_last >> 8, x_last};
    const uint8_t rows[] = {w.y_start >> 8, w.y_start, y_last >> 8, y_last};
    esp_err_t error = esp_lcd_panel_io_tx_param(io, CO5300_QSPI_COMMAND(0x2AU), columns, sizeof(columns));
    if (error != ESP_OK) return error;
    error = esp_lcd_panel_io_tx_param(io, CO5300_QSPI_COMMAND(0x2BU), rows, sizeof(rows));
    if (error != ESP_OK) return error;
    return esp_lcd_panel_io_tx_color(io, CO5300_QSPI_COLOR(0x2CU), pixels, bytes);
}
