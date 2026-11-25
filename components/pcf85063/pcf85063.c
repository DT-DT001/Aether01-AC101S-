#include "pcf85063.h"

#include <stdlib.h>
#include "esp_check.h"
#include "esp_log.h"

#define PCF85063_REG_CONTROL1 0x00
#define PCF85063_REG_SECONDS  0x04
#define PCF85063_TIMEOUT_MS   100

static const char *TAG = "pcf85063";

struct pcf85063_dev_t {
    i2c_master_dev_handle_t i2c_dev;
};

static uint8_t to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

static uint8_t from_bcd(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10U) + (value & 0x0F));
}

static esp_err_t read_regs(pcf85063_handle_t handle, uint8_t reg, uint8_t *data, size_t len,
                           int timeout_ms)
{
    return i2c_master_transmit_receive(handle->i2c_dev, &reg, 1, data, len, timeout_ms);
}

esp_err_t pcf85063_new(i2c_master_bus_handle_t bus, pcf85063_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(bus && out_handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    pcf85063_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "no memory");
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_I2C_ADDRESS,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &handle->i2c_dev);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }

    uint8_t control1 = 0;
    err = read_regs(handle, PCF85063_REG_CONTROL1, &control1, 1, PCF85063_TIMEOUT_MS);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(handle->i2c_dev);
        free(handle);
        return err;
    }
    ESP_LOGI(TAG, "PCF85063 detected at 0x%02X, control1=0x%02X", PCF85063_I2C_ADDRESS, control1);
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t pcf85063_del(pcf85063_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    esp_err_t err = i2c_master_bus_rm_device(handle->i2c_dev);
    free(handle);
    return err;
}

esp_err_t pcf85063_get_time_with_timeout(pcf85063_handle_t handle, pcf85063_time_t *time,
                                        int timeout_ms)
{
    if (!handle || !time || timeout_ms < 0) return ESP_ERR_INVALID_ARG;
    uint8_t data[7];
    const esp_err_t error = read_regs(handle, PCF85063_REG_SECONDS, data, sizeof(data), timeout_ms);
    if (error != ESP_OK) return error;
    time->oscillator_stopped = (data[0] & 0x80) != 0;
    time->second = from_bcd(data[0] & 0x7F);
    time->minute = from_bcd(data[1] & 0x7F);
    time->hour = from_bcd(data[2] & 0x3F);
    time->day = from_bcd(data[3] & 0x3F);
    time->weekday = data[4] & 0x07;
    time->month = from_bcd(data[5] & 0x1F);
    time->year = (uint16_t)(2000 + from_bcd(data[6]));
    return ESP_OK;
}

esp_err_t pcf85063_get_time(pcf85063_handle_t handle, pcf85063_time_t *time)
{
    ESP_RETURN_ON_FALSE(handle && time, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_ERROR(pcf85063_get_time_with_timeout(handle, time, PCF85063_TIMEOUT_MS), TAG, "read time");
    return ESP_OK;
}

esp_err_t pcf85063_set_time(pcf85063_handle_t handle, const pcf85063_time_t *time)
{
    ESP_RETURN_ON_FALSE(handle && time, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(time->year >= 2000 && time->year <= 2099 &&
                        time->month >= 1 && time->month <= 12 &&
                        time->day >= 1 && time->day <= 31 && time->weekday <= 6 &&
                        time->hour <= 23 && time->minute <= 59 && time->second <= 59,
                        ESP_ERR_INVALID_ARG, TAG, "invalid calendar value");
    const uint8_t data[] = {
        PCF85063_REG_SECONDS,
        to_bcd(time->second), to_bcd(time->minute), to_bcd(time->hour),
        to_bcd(time->day), time->weekday, to_bcd(time->month),
        to_bcd((uint8_t)(time->year - 2000)),
    };
    return i2c_master_transmit(handle->i2c_dev, data, sizeof(data), PCF85063_TIMEOUT_MS);
}
