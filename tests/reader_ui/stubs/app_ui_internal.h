#pragma once
#include "lvgl.h"
#include "app_io.h"
#include <stdarg.h>

typedef struct app_ui app_ui_t;
struct app_ui {
    lv_obj_t *screen, *content;
    void *page_context;
    void (*page_cleanup)(app_ui_t *);
    void (*page_back)(app_ui_t *);
    app_io_handle_t io;
    aether_storage_handle_t storage;
    aether_audio_player_handle_t audio_player;
    uint32_t page_generation;
    lv_timer_t *timer;
};
const lv_font_t *app_ui_chinese_font_size(unsigned px);
const lv_font_t *app_ui_chinese_font(void);
lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title);
lv_obj_t *app_ui_add_text(lv_obj_t *, const char *, lv_align_t, int32_t, int32_t, uint32_t, const lv_font_t *);
void app_ui_label_text(lv_obj_t *, const char *);
void app_ui_label_fmt(lv_obj_t *, const char *, ...);
void app_ui_request_io_ex(app_ui_t *, const app_io_request_t *, void (*)(app_ui_t *, const app_io_result_t *));
void app_ui_set_timer(app_ui_t *, lv_timer_cb_t, uint32_t);
void app_open_reader(app_ui_t *ui);
void app_open_reader_path(app_ui_t *ui, const char *path);
void app_open_music(app_ui_t *ui);
void app_open_music_path(app_ui_t *ui, const char *path);
size_t strlcpy(char *destination, const char *source, size_t size);
