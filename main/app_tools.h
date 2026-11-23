#pragma once

#include "aether_board.h"
#include "aether_audio_player.h"

typedef enum {
    APP_TOOLS_ARCADE_TILES = 1U << 0,
    APP_TOOLS_ARCADE_SNAKE = 1U << 1,
} app_tools_arcade_control_t;

typedef struct {
    uint8_t tilt_modes;
    uint8_t crates_level;
} app_tools_arcade_preferences_t;

typedef struct {
    bool ready;
    bool preferences_pending;
    bool preferences_deferred;
    bool clock_pending;
    bool network_clock_pending;
    bool network_clock_received;
    uint32_t best_score;
    float reference_pressure_pa;
    app_tools_arcade_preferences_t arcade;
    esp_err_t preferences_error;
    esp_err_t clock_error;
    uint32_t clock_revision;
    esp_err_t network_clock_error;
    uint32_t network_clock_revision;
    pcf85063_time_t clock_verified;
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
/* The clock app stores local Beijing time (UTC+8) in the RTC. The system
 * epoch remains UTC. These commands never wait for I2C or the network. */
bool app_tools_clock_network_ready(void);
esp_err_t app_tools_sync_clock(void);
void app_tools_set_clock_quiesced(bool quiesced);
esp_err_t app_tools_set_reference_pressure(float pressure_pa);
void app_tools_record_score(uint32_t score);
app_tools_arcade_preferences_t app_tools_get_arcade_preferences(void);
void app_tools_set_arcade_tilt(app_tools_arcade_control_t control, bool tilt);
void app_tools_set_crates_level(uint8_t level);
