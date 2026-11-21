#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
typedef void *esp_lcd_panel_io_handle_t;
typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(void *,void *,void *);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t,int,const void *,size_t);
