#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float up[3];
    float velocity[2], fraction[2];
    uint32_t stamp;
    bool ready;
} app_air_mouse_t;

/* Uses calibrated body-axis angular rates in deg/s and acceleration in m/s².
 * The display normal is +Z. A fresh start/gap never generates a jump. */
bool app_air_mouse_update(app_air_mouse_t *mouse, const float accel[3],
                          const float gyro[3], bool stationary, uint32_t stamp,
                          unsigned sensitivity_percent, int8_t *dx, int8_t *dy);
