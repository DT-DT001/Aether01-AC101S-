#pragma once
#include "app_ui.h"

struct app_ui {
    lv_obj_t *screen;
    lv_timer_t *timer;
    void *page_context;
    void (*page_cleanup)(app_ui_t *);
    app_power_t *power;
    bool media_active;
};

const lv_font_t *app_ui_chinese_font(void);
const lv_font_t *app_ui_chinese_font_size(unsigned size);
lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title);
lv_obj_t *app_ui_add_text(lv_obj_t *parent, const char *text,
                          lv_align_t align, int32_t x, int32_t y, uint32_t color,
                          const lv_font_t *font);
void app_ui_set_timer(app_ui_t *ui, lv_timer_cb_t callback, uint32_t period);
void app_ui_label_text(lv_obj_t *label, const char *text);
void app_ui_label_fmt(lv_obj_t *label, const char *format, ...);
bool app_ui_input_blocked(const app_ui_t *ui);
