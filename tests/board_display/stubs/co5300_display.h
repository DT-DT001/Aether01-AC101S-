#pragma once
#include "aether_board.h"
#include "driver/gpio.h"
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t, int, const void *, size_t);
esp_err_t co5300_display_set_sleep(esp_lcd_panel_io_handle_t, bool);
void co5300_display_cancel_te(esp_lcd_panel_io_handle_t);
