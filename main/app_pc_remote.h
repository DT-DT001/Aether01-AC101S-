#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "aether_bluetooth.h"
#include "app_power.h"

#define APP_PC_REMOTE_TEXT_MAX 48U
#define APP_PC_REMOTE_SCENES 4U

typedef struct {
    bool available, requested, discoverable, connected, reconnecting, confirm_pending;
    bool gyro_ready, gyro_calibrated, save_pending;
    uint32_t confirm_code;
    esp_err_t last_error, save_error;
} app_pc_remote_status_t;

typedef struct {
    uint8_t modifiers, key;
} app_pc_shortcut_t;

void app_pc_remote_init(aether_bluetooth_handle_t bluetooth, app_power_t *power,
                        bool imu_ready, bool (*flash_busy)(void *));
void app_pc_remote_get_status(app_pc_remote_status_t *status);
esp_err_t app_pc_remote_set_enabled(bool enabled);
esp_err_t app_pc_remote_confirm(bool accept);
esp_err_t app_pc_remote_key(uint8_t modifiers, uint8_t key);
esp_err_t app_pc_remote_text(const char *ascii);
esp_err_t app_pc_remote_click(uint8_t buttons);
void app_pc_remote_set_air_mode(bool enabled);
void app_pc_remote_set_motion(bool enabled);
void app_pc_remote_recenter(void);
void app_pc_remote_leave(void);
esp_err_t app_pc_remote_get_shortcut(unsigned slot, app_pc_shortcut_t *shortcut);
esp_err_t app_pc_remote_set_shortcut(unsigned slot, app_pc_shortcut_t shortcut);
esp_err_t app_pc_remote_trigger(unsigned slot);
bool app_pc_remote_save_pending(void);
