#pragma once

#include <stdint.h>
#include "app_ui.h"
#include "lvgl.h"

/* Load settings and the Flash wallpaper before creating the UI. */
void app_theme_init(void);
uint32_t app_theme_color(uint32_t fallback);
uint32_t app_theme_background(uint32_t fallback);

/* UI task only. Prefer the view helper if keeping the source in an LVGL object. */
const lv_image_dsc_t *app_theme_wallpaper(void);
lv_obj_t *app_theme_wallpaper_view(lv_obj_t *parent);
void app_open_theme(app_ui_t *ui);

/* Theme's TF-only gallery picker. A successful write submission owns pixels. */
void app_open_wallpaper_picker(app_ui_t *ui);
esp_err_t app_theme_write_wallpaper(uint16_t *pixels, uint16_t width, uint16_t height);
esp_err_t app_theme_write_status(void);
bool app_theme_wallpaper_available(void);
