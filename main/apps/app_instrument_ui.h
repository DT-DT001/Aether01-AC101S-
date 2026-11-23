#pragma once

#include <app_ui_internal.h>

enum {
    INSTRUMENT_PANEL = 0x1A2438,
    INSTRUMENT_BORDER = 0x303B57,
    INSTRUMENT_TEXT = 0xEAF2FF,
    INSTRUMENT_MUTED = 0x9EB3CD,
    INSTRUMENT_BLUE = 0x8FD7FF,
    INSTRUMENT_TEAL = 0x86E3C9,
    INSTRUMENT_PRIMARY = 0x2768D8,
    INSTRUMENT_SECONDARY = 0x233954,
};

static inline lv_obj_t *instrument_card(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(INSTRUMENT_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(INSTRUMENT_BORDER), 0);
    lv_obj_set_style_border_post(card, true, 0);
    return card;
}

static inline void instrument_button_style(lv_obj_t *button, uint32_t color)
{
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x38699B), LV_STATE_PRESSED);
}

static inline lv_obj_t *instrument_text(lv_obj_t *parent, const char *value, int x, int y,
                                        int width, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = app_ui_add_text(parent, value, LV_ALIGN_TOP_LEFT, x, y, color, font);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    return label;
}
