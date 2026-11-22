#pragma once

#include "lvgl.h"

typedef void (*app_text_input_layout_t)(void *context, bool visible);

/* Parent owns both the keyboard and its hot-plug timer. Keep the target
 * textarea in the same page, and use its READY/CANCEL events for actions. */
lv_obj_t *app_text_input_create(lv_obj_t *parent, lv_obj_t *textarea, bool pinyin,
                                app_text_input_layout_t layout, void *context);
void app_text_input_show(lv_obj_t *dock, bool show);
