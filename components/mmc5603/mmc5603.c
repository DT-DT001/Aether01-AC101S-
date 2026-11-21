#include "mmc5603.h"

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

#define MMC5603_REG_XOUT0       0x00
#define MMC5603_REG_STATUS1     0x18
#define MMC5603_REG_CTRL0       0x1B
#define MMC5603_REG_CTRL1       0x1C
#define MMC5603_REG_PRODUCT_ID  0x39
#define MMC5603_PRODUCT_ID      0x10
#define MMC5603_TIMEOUT_MS      100

static const char *TAG = "mmc5603";

struct mmc5603_dev_t {
    i2c_master_dev_handle_t i2c_dev;
};

static esp_err_t read_regs(mmc5603_handle_t handle, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(handle->i2c_dev, &reg, 1, data, len, MMC5603_TIMEOUT_MS);
}

static esp_err_t write_reg(mmc5603_handle_t handle, uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(handle->i2c_dev, data, sizeof(data), MMC5603_TIMEOUT_MS);
}

esp_err_t mmc5603_new(i2c_master_bus_handle_t bus, mmc5603_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(bus && out_handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    mmc5603_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "no memory");
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MMC5603_I2C_ADDRESS,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &handle->i2c_dev);
    if (ret != ESP_OK) {
        free(handle);
        return ret;
    }

    uint8_t product_id = 0;
    ret = read_regs(handle, MMC5603_REG_PRODUCT_ID, &product_id, 1);
    if (ret != ESP_OK || product_id != MMC5603_PRODUCT_ID) {
        ESP_LOGE(TAG, "unexpected product ID 0x%02X", product_id);
        i2c_master_bus_rm_device(handle->i2c_dev);
        free(handle);
        return ret == ESP_OK ? ESP_ERR_INVALID_RESPONSE : ret;
    }

    /* Bandwidth selection; measurements remain on-demand to minimize power. */
    ESP_GOTO_ON_ERROR(write_reg(handle, MMC5603_REG_CTRL1, 0x01), fail, TAG, "set bandwidth");
    ESP_LOGI(TAG, "MMC5603NJ detected at 0x%02X", MMC5603_I2C_ADDRESS);
    *out_handle = handle;
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(handle->i2c_dev);
    free(handle);
    return ret;
}

esp_err_t mmc5603_del(mmc5603_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    esp_err_t err = i2c_master_bus_rm_device(handle->i2c_dev);
    free(handle);
    return err;
}

esp_err_t mmc5603_read(mmc5603_handle_t handle, mmc5603_sample_t *sample)
{
    ESP_RETURN_ON_FALSE(handle && sample, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_ERROR(write_reg(handle, MMC5603_REG_CTRL0, 0x01), TAG, "trigger measurement");

    uint8_t status = 0;
    for (int retry = 0; retry < 20; ++retry) {
        ESP_RETURN_ON_ERROR(read_regs(handle, MMC5603_REG_STATUS1, &status, 1), TAG, "read status");
        if (status & 0x40) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_RETURN_ON_FALSE(status & 0x40, ESP_ERR_TIMEOUT, TAG, "measurement timeout");

    uint8_t raw[9];
    ESP_RETURN_ON_ERROR(read_regs(handle, MMC5603_REG_XOUT0, raw, sizeof(raw)), TAG, "read magnetic data");
    const uint32_t x = ((uint32_t)raw[0] << 12) | ((uint32_t)raw[1] << 4) | (raw[6] >> 4);
    const uint32_t y = ((uint32_t)raw[2] << 12) | ((uint32_t)raw[3] << 4) | (raw[7] >> 4);
    const uint32_t z = ((uint32_t)raw[4] << 12) | ((uint32_t)raw[5] << 4) | (raw[8] >> 4);
    sample->magnetic_ut[0] = ((int32_t)x - 524288) * 0.00625f;
    sample->magnetic_ut[1] = ((int32_t)y - 524288) * 0.00625f;
    sample->magnetic_ut[2] = ((int32_t)z - 524288) * 0.00625f;
    return ESP_OK;
}
