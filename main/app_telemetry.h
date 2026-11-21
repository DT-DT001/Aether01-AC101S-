#pragma once

#include <stdint.h>
#include "aether_board.h"

enum {
    APP_TELEMETRY_IMU = 1U << 0,
    APP_TELEMETRY_BAROMETER = 1U << 2,
};

typedef struct {
    qmi8658a_sample_t imu;
    dps368_sample_t barometer;
    pcf85063_time_t rtc;
    esp_err_t imu_error;
    esp_err_t barometer_error;
    esp_err_t rtc_error;
    uint32_t imu_updated_ms;
    uint32_t barometer_updated_ms;
    uint32_t rtc_updated_ms;
} app_telemetry_snapshot_t;

/** Start the sole periodic sensor/RTC reader. No I2C runs in the UI task. */
esp_err_t app_telemetry_start(aether_board_t *board);
/* Request a bounded startup calibration and stationary residual summary.
 * Call before start. The regular page subscriptions remain independent. */
void app_telemetry_warm_imu(void);
void app_telemetry_set_interest(uint32_t mask);
void app_telemetry_get(app_telemetry_snapshot_t *snapshot);
/* Background consumer. NULL sample during I2C quiescence. */
void app_telemetry_set_consumer(void (*consumer)(const app_telemetry_snapshot_t *));
/* Request high-rate samples from the shared IMU reader. Both callbacks run
 * on telemetry's task. The consumer receives NULL on quiesce. */
void app_telemetry_set_imu_measurement(bool (*requested)(void),
                                      void (*consume)(const app_telemetry_snapshot_t *));
/* Optional cached-state diagnostic on this background task, never LVGL. */
void app_telemetry_set_media_probe(void (*probe)(void *), void *context);
/** Stop all periodic I2C sampling and wait for the current read batch to exit.
 * Timeout leaves sampling gated; call resume after any aborted shutdown. */
esp_err_t app_telemetry_quiesce(uint32_t timeout_ms);
/** Resume after sensor restoration; cached samples become unavailable. */
void app_telemetry_resume(void);
