#pragma once
#include "lvgl.h"
#include "app_io.h"
#include "app_power.h"
#include <stdarg.h>
#include <stdatomic.h>
typedef struct app_ui app_ui_t;
struct app_ui {
    lv_obj_t *screen,*content,*app_obj[12];
    void *page_context;
    void (*page_cleanup)(app_ui_t *);
    app_io_handle_t io;
    aether_storage_handle_t storage;
    aether_audio_player_handle_t audio_player;
    app_power_t *power;
    uint32_t page_generation,app_value;
    lv_timer_t *timer;
    lv_obj_t *shutdown_overlay;
    atomic_bool storage_resume_pending;
    bool shutdown_failure_shown;
};
const lv_font_t *app_ui_chinese_font_size(unsigned);
const lv_font_t *app_ui_chinese_font(void);
lv_obj_t *app_ui_begin_page(app_ui_t *,const char *);
lv_obj_t *app_ui_add_text(lv_obj_t *,const char *,lv_align_t,int32_t,int32_t,uint32_t,const lv_font_t *);
void app_ui_label_text(lv_obj_t *,const char *);
void app_ui_label_fmt(lv_obj_t *,const char *,...);
void app_ui_set_timer(app_ui_t *,lv_timer_cb_t,uint32_t);
void app_ui_request_io_ex(app_ui_t *,const app_io_request_t *,void (*)(app_ui_t *,const app_io_result_t *));
void app_open_settings(app_ui_t *);
void app_open_music(app_ui_t *);
void app_open_usb(app_ui_t *);
esp_err_t app_ui_usb_set_enabled(app_ui_t *,bool);
void app_usb_storage_notice(app_ui_t *);
void app_ui_media_control_gate(app_ui_t *,bool,bool);
void app_ui_show_home(app_ui_t *);
bool lvgl_port_lock(uint32_t);
void lvgl_port_unlock(void);
esp_err_t app_ui_quiesce_storage_begin(app_ui_t *);
esp_err_t app_ui_quiesce_storage_wait(app_ui_t *,uint32_t);
esp_err_t app_ui_quiesce_storage_resume(app_ui_t *);
void test_ui_shutdown_service(app_ui_t *);
size_t strlcpy(char *,const char *,size_t);

void app_open_nes_controls(app_ui_t *ui);
