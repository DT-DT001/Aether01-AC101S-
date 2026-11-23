#pragma once

#include "app_ui_internal.h"

enum {
    CONNECTION_PANEL = 0x172336,
    CONNECTION_BORDER = 0x293C55,
    CONNECTION_CONTROL = 0x263A53,
    CONNECTION_BLUE = 0x245D9E,
    CONNECTION_TEXT = 0xEAF2FF,
    CONNECTION_DETAIL = 0xA7BED8,
    CONNECTION_ACCENT = 0x8BDBE0,
    CONNECTION_WARNING = 0xFFB992,
};

static inline void app_connection_card_style(lv_obj_t *card)
{
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(CONNECTION_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(CONNECTION_BORDER), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
}

static inline void app_connection_button_style(lv_obj_t *button, bool primary)
{
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? CONNECTION_BLUE : CONNECTION_CONTROL), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x355C82), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x283244), LV_STATE_DISABLED);
    lv_obj_set_style_recolor_opa(button, LV_OPA_TRANSP, LV_STATE_DISABLED);
    lv_obj_set_style_opa(button, LV_OPA_60, LV_STATE_DISABLED);
}
