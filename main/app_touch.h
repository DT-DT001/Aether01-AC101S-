#pragma once

#include "app_power.h"
#include "app_wake_input.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

/* Local nonfatal input adapter. Call with LVGL initialized. */
lv_indev_t *app_touch_add(lv_display_t *display, esp_lcd_touch_handle_t touch,
                          app_power_t *power);
/* LVGL owner only. Use when handing raw touch ownership to/from NES. */
void app_touch_set_enabled(lv_indev_t *indev, bool enabled);

/** Call under the LVGL lock. Returns whether the board touch input was enabled. */
bool app_touch_set_shutdown_paused(bool paused);

/* NES's raw reader shares the touch bus ownership lock with the other paths. */
esp_err_t app_touch_read_nes(void *context, esp_lcd_touch_point_data_t *points,
                             uint8_t *count, uint8_t capacity);
