#pragma once

#include "app_ui.h"
#include "lvgl.h"
#include "app_io.h"
#include "app_frames.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdatomic.h>

typedef struct {
    char directory[192];
    size_t exit_path_length;
    int32_t scroll_y;
    uint32_t media_generation;
    void (*parent)(app_ui_t *ui);
} app_files_return_t;

struct app_ui {
    aether_board_t *board;
    aether_storage_handle_t storage;
    aether_audio_player_handle_t audio_player;
    aether_video_player_handle_t video_player;
    aether_nes_handle_t nes;
    lv_indev_t *touch_indev;
    app_power_t *power;
    lv_obj_t *screen;
    lv_obj_t *content;
    lv_obj_t *home_time;
    lv_indev_t *home_swipe_input;
    lv_point_t home_swipe_origin;
    int32_t drawer_scroll_y;
    size_t drawer_focus_index;
    lv_obj_t *app_obj[12];
    char app_path[192];
    uint32_t app_value;
    lv_timer_t *timer;
    void (*page_cleanup)(app_ui_t *ui);
    bool (*page_can_exit)(app_ui_t *ui, void (*destination)(app_ui_t *));
    void (*page_back)(app_ui_t *ui);
    void *page_context;
    app_files_return_t files_return;
    app_io_handle_t io;
    uint32_t page_generation;
    void (*io_complete)(app_ui_t *ui, const app_io_result_t *result);
    app_frames_handle_t frames;
    lv_timer_t *service_timer;
    bool touch_restore_pending;
    bool media_active;
    bool media_started;
    QueueHandle_t media_control_queue;
    portMUX_TYPE media_control_lock;
    uint32_t media_control_epoch;
    bool media_control_blocked;
    bool io_media_pending;
    lv_obj_t *shutdown_overlay;
    atomic_bool storage_resume_pending;
    bool shutdown_failure_shown;
    bool user_key_exit_armed, user_key_exit_pending;
    int64_t user_key_armed_at_us;
    uint32_t user_key_target_generation, user_key_requested_generation;
    lv_obj_t *media_release_notice;
    uint32_t display_drops_seen, display_retry_at;
};

extern const lv_font_t font_16;
extern const lv_font_t font_24;

const lv_font_t *app_ui_chinese_font(void);
const lv_font_t *app_ui_chinese_font_size(unsigned px);
lv_obj_t *app_ui_begin_page(app_ui_t *ui, const char *title);
lv_obj_t *app_ui_new_screen(app_ui_t *ui);
void app_ui_show_drawer(app_ui_t *ui);
bool app_ui_go_back(app_ui_t *ui);
lv_obj_t *app_ui_add_text(lv_obj_t *parent, const char *text,
                          lv_align_t align, int32_t x, int32_t y,
                          uint32_t color, const lv_font_t *font);
void app_ui_set_timer(app_ui_t *ui, lv_timer_cb_t callback,
                      uint32_t period_ms);
void app_ui_clear_timer(app_ui_t *ui);
esp_err_t app_ui_media_control_init(app_ui_t *ui);
void app_ui_media_control_delete(app_ui_t *ui);
void app_ui_media_control_gate(app_ui_t *ui, bool blocked, bool invalidate);
void app_ui_media_control_process(app_ui_t *ui);
void app_ui_label_text(lv_obj_t *label, const char *text);
void app_ui_label_fmt(lv_obj_t *label, const char *format, ...) __attribute__((format(printf, 2, 3)));
void app_ui_request_io(app_ui_t *ui, app_io_operation_t operation,
                        void (*complete)(app_ui_t *, const app_io_result_t *));
void app_ui_request_io_ex(app_ui_t *ui, const app_io_request_t *request,
                         void (*complete)(app_ui_t *, const app_io_result_t *));
void app_open_reader_path(app_ui_t *ui, const char *path);
void app_open_music_path(app_ui_t *ui, const char *path);
void app_open_video_path(app_ui_t *ui, const char *path);
void app_open_nes_path(app_ui_t *ui, const char *path);

void app_open_status(app_ui_t *ui);
void app_open_clock(app_ui_t *ui);
void app_open_notes(app_ui_t *ui);
void app_open_calendar(app_ui_t *ui);
void app_open_weather(app_ui_t *ui);
void app_open_motion(app_ui_t *ui);
void app_open_displacement(app_ui_t *ui);
void app_open_gps(app_ui_t *ui);
void app_open_barometer(app_ui_t *ui);
void app_open_music(app_ui_t *ui);
void app_open_video(app_ui_t *ui);
void app_open_camera(app_ui_t *ui);
void app_open_gallery(app_ui_t *ui);
void app_open_reader(app_ui_t *ui);
void app_open_files(app_ui_t *ui);
void app_open_files_path(app_ui_t *ui, const char *path);
void app_open_recorder(app_ui_t *ui);
void app_open_game(app_ui_t *ui);
void app_open_nes(app_ui_t *ui);
void app_open_settings(app_ui_t *ui);
void app_open_wifi(app_ui_t *ui);
void app_open_intercom(app_ui_t *ui);
void app_open_bluetooth(app_ui_t *ui);
void app_open_pc_remote(app_ui_t *ui);
void app_open_obs_switcher(app_ui_t *ui);
void app_open_usb(app_ui_t *ui);
esp_err_t app_ui_usb_set_enabled(app_ui_t *ui, bool enabled);
void app_usb_storage_notice(app_ui_t *ui);

void app_ui_set_user_key_exit(app_ui_t *ui, bool enabled);
void app_open_nes_controls(app_ui_t *ui);
