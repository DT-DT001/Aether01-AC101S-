#pragma once
#include "metrics_runtime.h"
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t, int, int, int, int, const void *);
extern esp_err_t (*test_panel_draw)(esp_lcd_panel_handle_t, int, int, int, int, const void *);
