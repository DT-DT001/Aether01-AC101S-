#pragma once
#include "lvgl.h"
#include "esp_err.h"
#include <stdarg.h>
typedef struct { bool sleeping; } app_power_t;
typedef struct app_ui app_ui_t;
struct app_ui {
    app_power_t *power;
    lv_obj_t *screen, *content;
    lv_timer_t *timer;
    void *page_context;
    void (*page_cleanup)(app_ui_t *);
    void (*page_back)(app_ui_t *);
};
bool app_power_is_sleeping(const app_power_t *);
const lv_font_t *app_ui_chinese_font(void);
const lv_font_t *app_ui_chinese_font_size(unsigned);
lv_obj_t *app_ui_begin_page(app_ui_t *,const char *);
lv_obj_t *app_ui_add_text(lv_obj_t *,const char *,lv_align_t,int32_t,int32_t,uint32_t,const lv_font_t *);
void app_ui_label_text(lv_obj_t *,const char *);
void app_ui_label_fmt(lv_obj_t *,const char *,...) __attribute__((format(printf,2,3)));
void app_ui_set_timer(app_ui_t *,lv_timer_cb_t,uint32_t);
void app_open_motion(app_ui_t *);
