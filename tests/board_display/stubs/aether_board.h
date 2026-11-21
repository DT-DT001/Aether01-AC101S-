#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *esp_lcd_panel_handle_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef void *esp_lcd_touch_handle_t;
typedef void *tg29_handle_t;
typedef enum { TG29_RAIL_CLDO3 } tg29_rail_t;
typedef struct {
    esp_lcd_panel_handle_t display;
    esp_lcd_panel_io_handle_t display_io;
    esp_lcd_touch_handle_t touch;
    tg29_handle_t pmic;
    bool display_ready, display_sleeping, display_needs_reinit;
    bool touch_ready, touch_sleeping;
    uint8_t display_brightness;
} aether_board_t;
esp_err_t tg29_set_rail_enabled(tg29_handle_t, tg29_rail_t, bool);
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t, bool);
esp_err_t esp_lcd_touch_enter_sleep(esp_lcd_touch_handle_t);
esp_err_t esp_lcd_touch_exit_sleep(esp_lcd_touch_handle_t);
esp_err_t aether_board_set_display_sleep(aether_board_t *, bool);
esp_err_t aether_board_set_display_brightness(aether_board_t *, uint8_t);
esp_err_t aether_board_set_touch_sleep(aether_board_t *, bool);
