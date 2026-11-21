#include "dps368.h"

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

#define DPS368_REG_PSR_B2    0x00
#define DPS368_REG_PRS_CFG   0x06
#define DPS368_REG_TMP_CFG   0x07
#define DPS368_REG_MEAS_CFG  0x08
#define DPS368_REG_CFG       0x09
#define DPS368_REG_PRODUCT   0x0D
#define DPS368_REG_COEF      0x10
#define DPS368_REG_COEF_SRCE 0x28
#define DPS368_TIMEOUT_MS    100

static const char *TAG = "dps368";

typedef struct {
    int16_t c0;
    int16_t c1;
    int32_t c00;
    int32_t c10;
    int16_t c01;
    int16_t c11;
    int16_t c20;
    int16_t c21;
    int16_t c30;
} dps368_calib_t;

struct dps368_dev_t {
    i2c_master_dev_handle_t i2c_dev;
    dps368_calib_t calib;
    float pressure_scale;
    float temperature_scale;
    bool restore_pending;
    uint8_t awake_mode;
};

static int32_t sign_extend(uint32_t value, unsigned bits)
{
    const uint32_t sign = 1U << (bits - 1U);
    return (int32_t)((value ^ sign) - sign);
}

static esp_err_t read_regs(dps368_handle_t handle, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(handle->i2c_dev, &reg, 1, data, len, DPS368_TIMEOUT_MS);
}

static esp_err_t write_reg(dps368_handle_t handle, uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(handle->i2c_dev, data, sizeof(data), DPS368_TIMEOUT_MS);
}

static esp_err_t wait_until_ready(dps368_handle_t handle)
{
    uint8_t status = 0;
    for (int retry = 0; retry < 20; ++retry) {
        ESP_RETURN_ON_ERROR(read_regs(handle, DPS368_REG_MEAS_CFG, &status, 1),
                            TAG, "read startup state");
        if ((status & 0xC0) == 0xC0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGE(TAG, "startup timeout, MEAS_CFG=0x%02X", status);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t read_calibration(dps368_handle_t handle)
{
    uint8_t c[18];
    ESP_RETURN_ON_ERROR(read_regs(handle, DPS368_REG_COEF, c, sizeof(c)), TAG, "read coefficients");
    handle->calib.c0 = (int16_t)sign_extend(((uint16_t)c[0] << 4) | (c[1] >> 4), 12);
    handle->calib.c1 = (int16_t)sign_extend(((uint16_t)(c[1] & 0x0F) << 8) | c[2], 12);
    handle->calib.c00 = sign_extend(((uint32_t)c[3] << 12) | ((uint32_t)c[4] << 4) | (c[5] >> 4), 20);
    handle->calib.c10 = sign_extend(((uint32_t)(c[5] & 0x0F) << 16) | ((uint32_t)c[6] << 8) | c[7], 20);
    handle->calib.c01 = (int16_t)(((uint16_t)c[8] << 8) | c[9]);
    handle->calib.c11 = (int16_t)(((uint16_t)c[10] << 8) | c[11]);
    handle->calib.c20 = (int16_t)(((uint16_t)c[12] << 8) | c[13]);
    handle->calib.c21 = (int16_t)(((uint16_t)c[14] << 8) | c[15]);
    handle->calib.c30 = (int16_t)(((uint16_t)c[16] << 8) | c[17]);
    return ESP_OK;
}

esp_err_t dps368_new(i2c_master_bus_handle_t bus, dps368_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(bus && out_handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    dps368_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "no memory");
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DPS368_I2C_ADDRESS,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &handle->i2c_dev);
    if (ret != ESP_OK) {
        free(handle);
        return ret;
    }

    uint8_t product = 0;
    ESP_GOTO_ON_ERROR(read_regs(handle, DPS368_REG_PRODUCT, &product, 1), fail, TAG, "read product ID");
    ESP_GOTO_ON_ERROR(wait_until_ready(handle), fail, TAG, "wait for sensor and coefficients");
    ESP_GOTO_ON_ERROR(read_calibration(handle), fail, TAG, "read calibration");

    uint8_t coef_source = 0;
    ESP_GOTO_ON_ERROR(read_regs(handle, DPS368_REG_COEF_SRCE, &coef_source, 1), fail, TAG, "read temp source");
    /* 16 measurements/s, 8x oversampling for both channels. */
    ESP_GOTO_ON_ERROR(write_reg(handle, DPS368_REG_PRS_CFG, 0x43), fail, TAG, "pressure config");
    ESP_GOTO_ON_ERROR(write_reg(handle, DPS368_REG_TMP_CFG, (uint8_t)(0x43 | (coef_source & 0x80))), fail, TAG, "temperature config");
    ESP_GOTO_ON_ERROR(write_reg(handle, DPS368_REG_CFG, 0x00), fail, TAG, "shift config");
    ESP_GOTO_ON_ERROR(write_reg(handle, DPS368_REG_MEAS_CFG, 0x07), fail, TAG, "continuous mode");
    handle->pressure_scale = 7864320.0f;
    handle->temperature_scale = 7864320.0f;

    ESP_LOGI(TAG, "DPS368 detected at 0x%02X, product=0x%02X", DPS368_I2C_ADDRESS, product);
    *out_handle = handle;
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(handle->i2c_dev);
    free(handle);
    return ret;
}

esp_err_t dps368_del(dps368_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    esp_err_t err = i2c_master_bus_rm_device(handle->i2c_dev);
    free(handle);
    return err;
}

esp_err_t dps368_read(dps368_handle_t handle, dps368_sample_t *sample)
{
    ESP_RETURN_ON_FALSE(handle && sample, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!handle->restore_pending, ESP_ERR_INVALID_STATE, TAG, "sensor asleep/restoration pending");
    uint8_t ready = 0;
    ESP_RETURN_ON_ERROR(read_regs(handle, DPS368_REG_MEAS_CFG, &ready, 1), TAG, "read ready state");
    ESP_RETURN_ON_FALSE((ready & 0x30) == 0x30, ESP_ERR_NOT_FINISHED, TAG, "sample not ready");

    uint8_t raw[6];
    ESP_RETURN_ON_ERROR(read_regs(handle, DPS368_REG_PSR_B2, raw, sizeof(raw)), TAG, "read sample");
    const int32_t p_raw = sign_extend(((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | raw[2], 24);
    const int32_t t_raw = sign_extend(((uint32_t)raw[3] << 16) | ((uint32_t)raw[4] << 8) | raw[5], 24);
    const float p = p_raw / handle->pressure_scale;
    const float t = t_raw / handle->temperature_scale;
    const dps368_calib_t *c = &handle->calib;

    sample->temperature_c = c->c0 * 0.5f + c->c1 * t;
    sample->pressure_pa = c->c00 + p * (c->c10 + p * (c->c20 + p * c->c30)) +
                          t * c->c01 + t * p * (c->c11 + p * c->c21);
    return ESP_OK;
}

esp_err_t dps368_set_sleep(dps368_handle_t handle, bool sleep)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    if (sleep && !handle->restore_pending) {
        uint8_t old = 0;
        ESP_RETURN_ON_ERROR(read_regs(handle, DPS368_REG_MEAS_CFG, &old, 1), TAG, "save measurement mode");
        handle->awake_mode = old & 0x07;
        handle->restore_pending = true;
    }
    if (!sleep && !handle->restore_pending) return ESP_OK;
    /* Infineon DPS368 v1.1 section 8.5 p33: only bits2:0 are writable;
     * 000 stops background measurements; bits7:4 are read-only ready status. */
    const uint8_t mode = sleep ? 0 : handle->awake_mode;
    ESP_RETURN_ON_ERROR(write_reg(handle, DPS368_REG_MEAS_CFG, mode), TAG, "set measurement mode");
    uint8_t actual = 0;
    ESP_RETURN_ON_ERROR(read_regs(handle, DPS368_REG_MEAS_CFG, &actual, 1), TAG, "verify measurement mode");
    ESP_RETURN_ON_FALSE((actual & 0x07) == mode, ESP_ERR_INVALID_RESPONSE, TAG, "measurement mode mismatch");
    if (sleep) {
        /* Allow an in-flight 8x conversion to finish before cutting MCU power. */
        vTaskDelay(pdMS_TO_TICKS(20));
    } else {
        handle->restore_pending = false;
    }
    return ESP_OK;
}
