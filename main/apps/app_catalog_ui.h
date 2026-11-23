#pragma once

#include "lvgl.h"

static inline void app_catalog_panel(lv_obj_t *panel)
{
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101A2A), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x26364D), 0);
}

static inline void app_catalog_control(lv_obj_t *control, uint32_t color)
{
    lv_obj_set_style_bg_color(control, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(0x345473), LV_STATE_PRESSED);
    lv_obj_set_style_recolor(control, lv_color_hex(0x101A2A), LV_STATE_DISABLED);
    lv_obj_set_style_recolor_opa(control, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_border_width(control, 1, 0);
    lv_obj_set_style_border_color(control, lv_color_hex(0x344A64), 0);
    lv_obj_set_style_shadow_width(control, 0, 0);
    lv_obj_set_style_radius(control, 13, 0);
}

static inline void app_catalog_row(lv_obj_t *row)
{
    app_catalog_control(row, 0x19273B);
    lv_obj_set_style_border_color(row, lv_color_hex(0x2A3B53), 0);
}

static inline void app_catalog_badge(lv_obj_t *label)
{
    lv_obj_set_size(label, 34, 34);
    lv_obj_set_style_pad_top(label, 6, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(0x243D54), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(label, 9, 0);
}
