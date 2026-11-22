#pragma once
#include "lvgl.h"
#include "aether_storage.h"
#include <stdarg.h>
typedef struct app_ui app_ui_t;
struct app_ui {
    aether_storage_handle_t storage;
    lv_obj_t *screen, *content, *home_time, *app_obj[12];
    lv_indev_t *home_swipe_input;
    lv_point_t home_swipe_origin;
    int32_t drawer_scroll_y;
    size_t drawer_focus_index;
    lv_timer_t *timer;
    bool user_key_exit_armed;
};
const lv_font_t *app_ui_chinese_font(void);
const lv_font_t *app_ui_chinese_font_size(unsigned);
lv_obj_t *app_ui_new_screen(app_ui_t *);
lv_obj_t *app_ui_add_text(lv_obj_t *, const char *, lv_align_t, int32_t, int32_t, uint32_t, const lv_font_t *);
void app_ui_label_text(lv_obj_t *, const char *);
void app_ui_label_fmt(lv_obj_t *, const char *, ...);
void app_ui_set_timer(app_ui_t *, lv_timer_cb_t, uint32_t);
void app_ui_show_home(app_ui_t *);
void app_ui_show_drawer(app_ui_t *);
void app_ui_set_user_key_exit(app_ui_t *, bool);
#define DECLARE_APP(name) void app_open_##name(app_ui_t *);
DECLARE_APP(status) DECLARE_APP(clock) DECLARE_APP(motion)
DECLARE_APP(barometer) DECLARE_APP(music) DECLARE_APP(video) DECLARE_APP(reader)
DECLARE_APP(files) DECLARE_APP(game) DECLARE_APP(nes) DECLARE_APP(settings)
DECLARE_APP(wifi) DECLARE_APP(bluetooth)
DECLARE_APP(recorder)
DECLARE_APP(pc_remote) DECLARE_APP(obs_switcher)
DECLARE_APP(displacement)
DECLARE_APP(notes) DECLARE_APP(calendar) DECLARE_APP(weather)
DECLARE_APP(intercom) DECLARE_APP(gps) DECLARE_APP(camera) DECLARE_APP(gallery)
