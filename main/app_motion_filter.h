#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    APP_MOTION_WAITING, APP_MOTION_STILL, APP_MOTION_TILTED, APP_MOTION_TURNING,
    APP_MOTION_MOVING, APP_MOTION_SHAKING, APP_MOTION_IMPACT, APP_MOTION_FREEFALL
} app_motion_action_t;
typedef struct {
    float q[4], reference[4], relative[4];
    float roll, pitch, yaw, acceleration, linear, rotation;
    float history[48];
    uint32_t stamp, shake_stamp, impact_stamp;
    unsigned history_next, reversals;
    int previous_sign;
    bool initialized, impact_active;
    app_motion_action_t action;
} app_motion_filter_t;
void app_motion_filter_init(app_motion_filter_t *f);
bool app_motion_filter_update(app_motion_filter_t *f, const float accel[3],
                              const float gyro[3], uint32_t timestamp_ms);
bool app_motion_filter_update_tilt(app_motion_filter_t *f, const float accel[3],
                                   const float gyro[3], uint32_t timestamp_ms,
                                   bool gyro_bias_valid);
void app_motion_filter_zero(app_motion_filter_t *f);
const char *app_motion_action_name(app_motion_action_t action);
