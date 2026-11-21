#pragma once
#include "lvgl.h"
#include "app_io.h"
#include <stdarg.h>
typedef struct app_ui app_ui_t;
struct app_ui {
    lv_obj_t *screen,*content;
    void *page_context;
    void (*page_cleanup)(app_ui_t *);
    aether_storage_handle_t storage;
    app_io_handle_t io;
    lv_timer_t *timer;
    void (*io_complete)(app_ui_t *,const app_io_result_t *);
};
const lv_font_t *app_ui_chinese_font(void);
const lv_font_t *app_ui_chinese_font_size(unsigned);
lv_obj_t *app_ui_begin_page(app_ui_t *,const char *);
lv_obj_t *app_ui_add_text(lv_obj_t *,const char *,lv_align_t,int32_t,int32_t,uint32_t,const lv_font_t *);
void app_ui_label_text(lv_obj_t *,const char *);
void app_ui_label_fmt(lv_obj_t *,const char *,...);
void app_ui_set_timer(app_ui_t *,lv_timer_cb_t,uint32_t);
void app_ui_request_io_ex(app_ui_t *,const app_io_request_t *,void (*)(app_ui_t *,const app_io_result_t *));
void app_open_files(app_ui_t *);
void app_open_files_path(app_ui_t *,const char *);
void app_open_reader_path(app_ui_t *,const char *);
void app_open_music_path(app_ui_t *,const char *);
void app_open_video_path(app_ui_t *,const char *);
void app_open_nes_path(app_ui_t *,const char *);
size_t strlcpy(char *,const char *,size_t);
