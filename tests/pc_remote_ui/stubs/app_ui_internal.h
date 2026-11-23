#pragma once
#include "lvgl.h"

typedef struct app_ui {
    lv_obj_t *screen, *content;
    lv_timer_t *timer;
    void *page_context;
    void (*page_cleanup)(struct app_ui *ui);
    void (*page_suspend)(struct app_ui *ui, bool suspended);
} app_ui_t;

const lv_font_t *app_ui_chinese_font(void);
const lv_font_t *app_ui_chinese_font_size(unsigned px);
lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title);
lv_obj_t *app_ui_add_text(lv_obj_t *, const char *, lv_align_t, int32_t, int32_t, uint32_t, const lv_font_t *);
void app_ui_label_text(lv_obj_t *, const char *);
void app_ui_label_fmt(lv_obj_t *, const char *, ...);
void app_ui_set_timer(app_ui_t *, lv_timer_cb_t, uint32_t);
void app_open_pc_remote(app_ui_t *ui);
void app_open_obs_switcher(app_ui_t *ui);
