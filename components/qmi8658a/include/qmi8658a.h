#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QMI8658A_I2C_ADDRESS 0x6B

typedef struct qmi8658a_dev_t *qmi8658a_handle_t;

typedef struct {
    float accel_mps2[3];
    float gyro_dps[3];      /* Raw minus the learned bias, in native sensor axes. */
    float gyro_raw_dps[3];
    float gyro_bias_dps[3]; /* Zero until the first stationary window qualifies. */
    bool gyro_bias_valid;   /* A learned estimate is available, including in motion. */
    bool gyro_stationary;   /* The current contiguous stationary window has qualified. */
} qmi8658a_sample_t;

typedef struct {
    float accel_reference[3];
    float bias_dps[3];
    uint32_t samples;
} qmi8658a_gyro_zero_t;

esp_err_t qmi8658a_new(i2c_master_bus_handle_t bus, qmi8658a_handle_t *out_handle);
esp_err_t qmi8658a_del(qmi8658a_handle_t handle);
/** Read one accel/gyro burst and update the shared bias estimate.
 * Use one sampling owner and share the resulting samples with consumers.
 * Gaps, read failures and sleep break stationary evidence. The last learned
 * bias is retained for this handle's lifetime. */
esp_err_t qmi8658a_read(qmi8658a_handle_t handle, qmi8658a_sample_t *sample);
/** For frequent sampling, require a new output counter and reject bursts that
 * cross an output update. ESP_ERR_NOT_FINISHED leaves sample untouched. */
esp_err_t qmi8658a_read_fresh(qmi8658a_handle_t handle, qmi8658a_sample_t *sample);
/** Check and average one distinct zero-rate sample. Movement leaves the
 * candidate unchanged. The caller schedules and owns the sample window. */
bool qmi8658a_gyro_zero_add(qmi8658a_gyro_zero_t *window, const qmi8658a_sample_t *sample);
/** Sampler-owner APIs. These change only RAM state and never access I2C. */
void qmi8658a_set_gyro_auto_bias(qmi8658a_handle_t handle, bool enabled);
esp_err_t qmi8658a_set_gyro_bias(qmi8658a_handle_t handle, const float bias_dps[3]);
/** Sole-owner API: quiesce sampling first. Enter documented Power Down
 * (CTRL7 aEN/gEN=0, CTRL1.SensorDisable=1) and verify both registers.
 * false restores the pre-sleep configuration, including after a partial error.
 * Register retention allows recovery without resetting the always-powered IC. */
esp_err_t qmi8658a_set_sleep(qmi8658a_handle_t handle, bool sleep);

#ifdef __cplusplus
}
#endif
