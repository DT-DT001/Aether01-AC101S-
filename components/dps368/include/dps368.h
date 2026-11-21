#pragma once

#include "driver/i2c_master.h"
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DPS368_I2C_ADDRESS 0x77

typedef struct dps368_dev_t *dps368_handle_t;

typedef struct {
    float pressure_pa;
    float temperature_c;
} dps368_sample_t;

esp_err_t dps368_new(i2c_master_bus_handle_t bus, dps368_handle_t *out_handle);
esp_err_t dps368_del(dps368_handle_t handle);
esp_err_t dps368_read(dps368_handle_t handle, dps368_sample_t *sample);
/** Sole-owner API: stop the sampler first. Enter standby (MEAS_CFG[2:0]=000)
 * with readback; false restores the saved background mode after success or a
 * partially failed sleep. Does not reset calibration or change oversampling. */
esp_err_t dps368_set_sleep(dps368_handle_t handle, bool sleep);

#ifdef __cplusplus
}
#endif
