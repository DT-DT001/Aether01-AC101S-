#pragma once

#include "aether_audio_player.h"
#include "aether_bluetooth.h"
#include "aether_board.h"
#include "aether_video_player.h"
#include "aether_nes.h"
#include "aether_storage.h"
#include "app_power.h"
#include "esp_err.h"
#include "lvgl.h"

typedef struct app_ui app_ui_t;

esp_err_t app_ui_start(aether_board_t *board,
                       aether_storage_handle_t storage,
                       aether_audio_player_handle_t audio_player,
                       aether_video_player_handle_t video_player,
                       aether_nes_handle_t nes,
                       lv_indev_t *touch_indev,
                       app_power_t *power,
                       app_ui_t **out_ui);

void app_ui_show_home(app_ui_t *ui);
/* Nonblocking cross-task handoff; execution and owner selection run on LVGL. */
esp_err_t app_ui_queue_media_control(app_ui_t *ui, aether_bluetooth_remote_action_t action);
/* Power-task entry points. BEGIN briefly takes LVGL to snapshot/close the old
 * page, but all media/storage waits run after releasing that lock. */
esp_err_t app_ui_quiesce_storage_begin(app_ui_t *ui);
esp_err_t app_ui_quiesce_storage_wait(app_ui_t *ui, uint32_t timeout_ms);
esp_err_t app_ui_quiesce_storage_resume(app_ui_t *ui);

/* Cache-only input gate, retained until an aborted shutdown resumes IO. */
bool app_ui_input_blocked(const app_ui_t *ui);

/* Power-task publication only; the GUI checks page identity before navigation. */
void app_ui_queue_user_key(void *context, int64_t pressed_at_us);
/* Temporary device probe; caller holds LVGL, follows normal slider submission. */
esp_err_t app_ui_video_probe_seek(app_ui_t *ui, uint32_t position_ms);
