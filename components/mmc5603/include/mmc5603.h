#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MMC5603_I2C_ADDRESS 0x30

typedef struct mmc5603_dev_t *mmc5603_handle_t;

typedef struct {
    float magnetic_ut[3];
} mmc5603_sample_t;

esp_err_t mmc5603_new(i2c_master_bus_handle_t bus, mmc5603_handle_t *out_handle);
esp_err_t mmc5603_del(mmc5603_handle_t handle);
esp_err_t mmc5603_read(mmc5603_handle_t handle, mmc5603_sample_t *sample);

#ifdef __cplusplus
}
#endif
