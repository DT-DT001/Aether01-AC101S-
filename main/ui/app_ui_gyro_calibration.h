#pragma once

#include "esp_err.h"
#include "lvgl.h"

typedef struct {
    lv_obj_t *box, *status, *action, *action_label;
    esp_err_t start_error;
    bool started;
} app_ui_gyro_calibration_t;

void app_ui_gyro_calibration_open(lv_obj_t *screen, app_ui_gyro_calibration_t *dialog);
void app_ui_gyro_calibration_refresh(app_ui_gyro_calibration_t *dialog);
void app_ui_gyro_calibration_close(app_ui_gyro_calibration_t *dialog);
