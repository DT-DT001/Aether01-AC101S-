#pragma once

#include "lvgl.h"

uint32_t app_theme_color(uint32_t fallback);
uint32_t app_theme_background(uint32_t fallback);
lv_obj_t *app_theme_wallpaper_view(lv_obj_t *parent);
