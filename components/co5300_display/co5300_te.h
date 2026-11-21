#pragma once
#include "co5300_display.h"
/* One fitted panel. Destroy only after its LVGL producer has stopped. */
esp_err_t co5300_te_init(esp_lcd_panel_io_handle_t io, gpio_num_t pin);
esp_err_t co5300_te_deinit(esp_lcd_panel_io_handle_t io);
