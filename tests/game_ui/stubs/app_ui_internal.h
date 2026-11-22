#pragma once

#include "lvgl.h"

typedef struct app_power app_power_t;
typedef struct app_ui {
    lv_obj_t *screen;
    lv_obj_t *content;
    lv_timer_t *timer;
    void *page_context;
    void (*page_cleanup)(struct app_ui *);
    void (*page_back)(struct app_ui *);
    app_power_t *power;
} app_ui_t;

lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title);
lv_obj_t *app_ui_add_text(lv_obj_t *, const char *, lv_align_t, int32_t, int32_t, uint32_t, const lv_font_t *);
const lv_font_t *app_ui_chinese_font(void);
const lv_font_t *app_ui_chinese_font_size(unsigned px);
void app_ui_label_text(lv_obj_t *label, const char *text);
void app_ui_label_fmt(lv_obj_t *label, const char *format, ...);
void app_ui_set_timer(app_ui_t *ui, lv_timer_cb_t callback, uint32_t period_ms);
void app_ui_clear_timer(app_ui_t *ui);
bool app_power_is_sleeping(const app_power_t *power);
void app_open_game(app_ui_t *ui);
