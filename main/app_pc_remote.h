#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "aether_bluetooth.h"
#include "app_power.h"

#define APP_PC_REMOTE_SCENES 4U
#define APP_PC_REMOTE_SCENE_CHARACTERS 16U
#define APP_PC_REMOTE_SCENE_BYTES (APP_PC_REMOTE_SCENE_CHARACTERS * 4U + 1U)
enum { APP_PC_MOUSE_MIN = 50, APP_PC_MOUSE_DEFAULT = 100, APP_PC_MOUSE_MAX = 200, APP_PC_MOUSE_STEP = 10 };

typedef struct {
    bool available, requested, discoverable, connected, reconnecting, confirm_pending;
    bool gyro_ready, gyro_calibrated, save_pending;
    uint32_t confirm_code;
    uint8_t sensitivity;
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
esp_err_t app_pc_remote_click(uint8_t buttons);
void app_pc_remote_set_air_mode(bool enabled);
void app_pc_remote_set_motion(bool enabled);
void app_pc_remote_recenter(void);
esp_err_t app_pc_remote_set_sensitivity(unsigned percent);
/* Release queued keys, buttons and IMU control without changing the link. */
void app_pc_remote_release_controls(void);
void app_pc_remote_shutdown(void);
esp_err_t app_pc_remote_get_shortcut(unsigned slot, app_pc_shortcut_t *shortcut);
esp_err_t app_pc_remote_set_shortcut(unsigned slot, app_pc_shortcut_t shortcut);
esp_err_t app_pc_remote_get_scene_name(unsigned slot, char name[APP_PC_REMOTE_SCENE_BYTES]);
esp_err_t app_pc_remote_set_scene_name(unsigned slot, const char *name);
esp_err_t app_pc_remote_trigger(unsigned slot);
bool app_pc_remote_save_pending(void);
