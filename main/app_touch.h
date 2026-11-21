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
