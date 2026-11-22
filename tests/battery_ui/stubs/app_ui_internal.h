#pragma once
#include "lvgl.h"
#include <stdarg.h>
typedef struct app_ui {
    lv_obj_t *screen, *content;
    bool (*page_can_exit)(struct app_ui *, void (*destination)(struct app_ui *));
    void (*page_back)(struct app_ui *);
} app_ui_t;
const lv_font_t *app_ui_chinese_font(void);
const lv_font_t *app_ui_chinese_font_size(unsigned);
lv_obj_t *app_ui_new_screen(app_ui_t *);
lv_obj_t *app_ui_begin_page(app_ui_t *, const char *);
lv_obj_t *app_ui_add_text(lv_obj_t *, const char *, lv_align_t, int32_t, int32_t, uint32_t, const lv_font_t *);
void app_ui_label_text(lv_obj_t *, const char *);
void app_ui_label_fmt(lv_obj_t *, const char *, ...);
void app_ui_show_drawer(app_ui_t *);
void app_ui_set_user_key_exit(app_ui_t *, bool);
