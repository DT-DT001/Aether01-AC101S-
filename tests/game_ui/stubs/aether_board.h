#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_NOT_FINISHED, ESP_ERR_TIMEOUT };
typedef struct { float accel_mps2[3], gyro_dps[3]; bool gyro_bias_valid; } qmi8658a_sample_t;
typedef struct { int unused; } aether_board_t;
typedef struct { int unused; } dps368_sample_t;
typedef struct { int unused; } pcf85063_time_t;
