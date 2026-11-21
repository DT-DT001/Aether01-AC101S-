#include "co5300_display.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* The same QSPI framing as the installed CO5300 driver: opcode 0x02 and
 * eight-bit DCS command in bits 15:8. Ordinary sleep never enters DSTBON. */
static esp_err_t dcs(esp_lcd_panel_io_handle_t io, unsigned command)
{
    return esp_lcd_panel_io_tx_param(io, (0x02U << 24) | (command << 8), NULL, 0);
}

esp_err_t co5300_display_set_sleep(esp_lcd_panel_io_handle_t io, bool sleeping)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    co5300_display_cancel_te(io);
    esp_err_t error;
    if (sleeping) {
        /* SPI tx_param drains queued color transfers before sending DCS. */
        error = dcs(io, 0x28); /* Display off. */
        if (error != ESP_OK) return error;
        error = dcs(io, 0x10); /* Sleep in. Keep hardware supplies/reset. */
    } else {
        error = dcs(io, 0x11); /* Sleep out. */
    }
    if (error != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(120));
    return sleeping ? ESP_OK : dcs(io, 0x29);
}
