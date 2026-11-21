#pragma once

#include "aether_board.h"
#include "aether_audio_player.h"

typedef struct {
    bool ready;
    bool preferences_pending;
    bool preferences_deferred;
    bool clock_pending;
    uint32_t best_score;
    float reference_pressure_pa;
    esp_err_t preferences_error;
    esp_err_t clock_error;
    uint32_t clock_revision;
    tg29_power_state_t power;
    esp_err_t power_error;
    uint32_t power_updated_ms;
    tg29_battery_state_t battery;
    esp_err_t battery_error;
    uint32_t battery_updated_ms; /* Last successful background sample. */
} app_tools_snapshot_t;

/* NVS and RTC/PMIC transactions execute exclusively in this task, never LVGL. */
esp_err_t app_tools_init(aether_board_t *board);
/* Keep preference writes in RAM while background music is producing audio. */
void app_tools_set_audio_player(aether_audio_player_handle_t audio);
void app_tools_set_persistence_guard(bool (*busy)(void *), void *context);
void app_tools_get_snapshot(app_tools_snapshot_t *snapshot);
esp_err_t app_tools_set_clock(const pcf85063_time_t *time);
esp_err_t app_tools_set_reference_pressure(float pressure_pa);
void app_tools_record_score(uint32_t score);
