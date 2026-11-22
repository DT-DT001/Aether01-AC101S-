#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t year;
    uint8_t month, day, hour, minute;
    bool oscillator_stopped;
} pcf85063_time_t;
typedef struct {
    esp_err_t rtc_error;
    pcf85063_time_t rtc;
} app_telemetry_snapshot_t;

void app_telemetry_get(app_telemetry_snapshot_t *out);
