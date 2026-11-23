#pragma once

#include "lvgl.h"

typedef void (*app_text_input_layout_t)(void *context, bool visible);

/* A focused caret needs its own contrast when a page overrides the
 * textarea background independently of the LVGL theme. */
static inline void app_text_input_style_cursor(lv_obj_t *textarea, uint32_t color)
{
    const lv_style_selector_t focused = LV_PART_CURSOR | LV_STATE_FOCUSED;
    lv_obj_set_style_border_side(textarea, LV_BORDER_SIDE_LEFT, focused);
    lv_obj_set_style_border_width(textarea, 3, focused);
    lv_obj_set_style_border_color(textarea, lv_color_hex(color), focused);
    lv_obj_set_style_border_opa(textarea, LV_OPA_COVER, focused);
    lv_obj_set_style_anim_duration(textarea, 500, focused);
    lv_obj_set_style_border_opa(textarea, LV_OPA_TRANSP, LV_PART_CURSOR | LV_STATE_DISABLED);
}

/* Parent owns both the keyboard and its hot-plug timer. Keep the target
 * textarea in the same page, and use its READY/CANCEL events for actions. */
lv_obj_t *app_text_input_create(lv_obj_t *parent, lv_obj_t *textarea, bool pinyin,
                                app_text_input_layout_t layout, void *context);
void app_text_input_show(lv_obj_t *dock, bool show);
