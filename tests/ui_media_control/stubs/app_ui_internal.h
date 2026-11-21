#pragma once
#include "app_io.h"
#include "app_media_control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
typedef void lv_timer_t;
typedef struct lv_obj { char text[256]; } lv_obj_t;
typedef int lv_font_t;
typedef int lv_align_t;
typedef int app_power_t;
typedef enum {
    APP_POWER_SHUTDOWN_IDLE, APP_POWER_SHUTDOWN_PREPARING,
    APP_POWER_SHUTDOWN_FAILED, APP_POWER_SHUTDOWN_REQUESTED,
} app_power_shutdown_state_t;
typedef struct {
    bool shutdown_recovering;
    app_power_shutdown_state_t shutdown_state;
} app_power_status_t;
#define LV_ALIGN_BOTTOM_MID 1
#define LV_TEXT_ALIGN_CENTER 2
#define LV_OPA_90 230
typedef struct app_ui app_ui_t;
struct app_ui {
    aether_audio_player_handle_t audio_player;
    aether_video_player_handle_t video_player;
    aether_nes_handle_t nes;
    QueueHandle_t media_control_queue;
    portMUX_TYPE media_control_lock;
    uint32_t media_control_epoch,page_generation;
    bool media_control_blocked,io_media_pending;
    void (*io_complete)(app_ui_t *,const app_io_result_t *);
    app_io_handle_t io;
    lv_timer_t *service_timer;
    app_power_t *power;
    lv_obj_t *screen, *media_release_notice;
    bool user_key_exit_pending;
    uint32_t user_key_requested_generation;
    bool test_input_blocked;
};
esp_err_t app_ui_media_control_init(app_ui_t *);
void app_ui_media_control_delete(app_ui_t *);
void app_ui_media_control_gate(app_ui_t *,bool,bool);
void app_ui_media_control_process(app_ui_t *);
esp_err_t app_ui_queue_media_control(app_ui_t *,aether_bluetooth_remote_action_t);
void app_ui_request_io_ex(app_ui_t *,const app_io_request_t *,void (*)(app_ui_t *,const app_io_result_t *));
void lv_timer_set_period(lv_timer_t *,uint32_t);
void lv_timer_ready(lv_timer_t *);
void *lv_timer_get_user_data(lv_timer_t *);
void test_service_timer(lv_timer_t *);
void app_usb_storage_notice(app_ui_t *);
esp_err_t app_power_get_status(const app_power_t *,app_power_status_t *);
bool app_ui_input_blocked(const app_ui_t *);
bool app_intercom_busy(void);
void app_ui_show_home(app_ui_t *);
lv_obj_t *app_ui_add_text(lv_obj_t *,const char *,lv_align_t,int32_t,int32_t,uint32_t,const lv_font_t *);
const lv_font_t *app_ui_chinese_font(void);
void app_ui_label_text(lv_obj_t *,const char *);
void app_ui_label_fmt(lv_obj_t *,const char *,...);
void lv_obj_set_size(lv_obj_t *,int32_t,int32_t);
void lv_obj_set_style_text_align(lv_obj_t *,int,int);
void lv_obj_set_style_bg_color(lv_obj_t *,uint32_t,int);
void lv_obj_set_style_bg_opa(lv_obj_t *,int,int);
uint32_t lv_color_hex(uint32_t);
void lv_obj_delete(lv_obj_t *);
