#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_ERR_NOT_FINISHED 0x10c
typedef struct {
    uint8_t brightness;
    uint32_t timeout;
} app_power_t;
typedef enum {APP_POWER_SHUTDOWN_IDLE,APP_POWER_SHUTDOWN_PREPARING,APP_POWER_SHUTDOWN_FAILED,APP_POWER_SHUTDOWN_REQUESTED} app_power_shutdown_state_t;
typedef enum {APP_POWER_SHUTDOWN_BEGIN,APP_POWER_SHUTDOWN_QUIESCE,APP_POWER_SHUTDOWN_SENSORS,APP_POWER_SHUTDOWN_FINALIZE,APP_POWER_SHUTDOWN_POWER_OFF,APP_POWER_SHUTDOWN_CANCEL} app_power_shutdown_stage_t;
typedef struct {bool persistence_pending,persistence_deferred,shutdown_recovering;esp_err_t display_error,persistence_error;app_power_shutdown_state_t shutdown_state;app_power_shutdown_stage_t shutdown_stage;esp_err_t shutdown_error,sensor_restore_error,shutdown_cancel_error;} app_power_status_t;
uint32_t app_power_get_timeout(const app_power_t *);
uint8_t app_power_get_brightness(const app_power_t *);
esp_err_t app_power_set_timeout(app_power_t *,uint32_t,bool);
esp_err_t app_power_set_brightness(app_power_t *,uint8_t,bool);
esp_err_t app_power_sleep_now(app_power_t *);
esp_err_t app_power_get_status(const app_power_t *,app_power_status_t *);
