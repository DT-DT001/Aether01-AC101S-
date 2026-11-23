#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    APP_GYRO_CAL_IDLE,
    APP_GYRO_CAL_COUNTDOWN,
    APP_GYRO_CAL_SAMPLING,
    APP_GYRO_CAL_SAVING,
    APP_GYRO_CAL_COMPLETE,
    APP_GYRO_CAL_FAILED,
    APP_GYRO_CAL_CANCELLED,
} app_gyro_calibration_phase_t;

typedef struct {
    app_gyro_calibration_phase_t phase;
    uint32_t remaining_ms, samples;
    esp_err_t error;
} app_gyro_calibration_status_t;

/** Commands are asynchronous. The telemetry task owns sampling and NVS. */
esp_err_t app_telemetry_gyro_calibration_begin(void);
/** Cancellation is available until the completed sample window starts saving. */
esp_err_t app_telemetry_gyro_calibration_cancel(void);
void app_telemetry_gyro_calibration_get(app_gyro_calibration_status_t *status);
