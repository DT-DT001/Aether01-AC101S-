#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "pcf85063.h"
typedef struct { pcf85063_time_t rtc; esp_err_t rtc_error; } app_telemetry_snapshot_t;
void app_telemetry_get(app_telemetry_snapshot_t *);
