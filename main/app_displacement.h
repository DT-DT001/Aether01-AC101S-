#pragma once

#include "app_displacement_estimator.h"
#include "app_power.h"

typedef struct {
    bool available, active;
    app_displacement_state_t state;
    app_displacement_result_t result;
} app_displacement_status_t;

void app_displacement_init(app_power_t *power, bool imu_ready);
void app_displacement_get_status(app_displacement_status_t *status);
esp_err_t app_displacement_start(void);
esp_err_t app_displacement_finish(void);
esp_err_t app_displacement_resume(void);
void app_displacement_cancel(void);
