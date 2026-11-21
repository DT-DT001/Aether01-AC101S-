#pragma once
#include "qmi8658a.h"
enum { APP_TELEMETRY_IMU = 1U << 0 };
typedef struct {
    qmi8658a_sample_t imu;
    esp_err_t imu_error;
    uint32_t imu_updated_ms;
} app_telemetry_snapshot_t;
void app_telemetry_get(app_telemetry_snapshot_t *);
void app_telemetry_set_interest(uint32_t);
