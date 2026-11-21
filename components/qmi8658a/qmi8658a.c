#include "qmi8658a.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define QMI8658_REG_WHO_AM_I 0x00
#define QMI8658_REG_CTRL1    0x02
#define QMI8658_REG_CTRL2    0x03
#define QMI8658_REG_CTRL3    0x04
#define QMI8658_REG_CTRL5    0x06
#define QMI8658_REG_CTRL7    0x08
#define QMI8658_REG_TIMESTAMP 0x30
#define QMI8658_REG_AX_L     0x35
#define QMI8658_SAMPLE_BYTES 12
#define QMI8658_COUNTER_BYTES 3
#define QMI8658_FAST_TIMEOUT_MS 12
#define QMI8658_CHIP_ID      0x05
#define QMI8658_TIMEOUT_MS   100

/* Conservative application thresholds for the 20ms telemetry sampling path.
 * An IMU alone cannot distinguish arbitrarily slow yaw from a small offset. */
#define GRAVITY_MPS2                9.80665f
#define GYRO_STILL_GRAVITY_TOL_MPS2 0.50f
#define GYRO_STILL_ACCEL_DELTA_MPS2 0.18f
#define GYRO_STILL_RAW_LIMIT_DPS   12.00f
#define GYRO_STILL_INITIAL_YAW_DPS 2.00f
#define GYRO_STILL_RESIDUAL_DPS     0.55f
#define GYRO_STILL_VARIATION_DPS    0.35f
#define GYRO_STILL_MIN_MS           1200U
#define GYRO_STILL_MIN_SAMPLES      40U
#define GYRO_STILL_MAX_GAP_MS       100U
#define GYRO_BIAS_TIME_CONSTANT_MS  6000U

static const char *TAG = "qmi8658a";

typedef struct {
    float bias[3];
    float accel_reference[3];
    float gyro_mean[3];
    TickType_t window_start;
    TickType_t last_sample;
    unsigned samples;
    bool valid;
    bool stationary;
} gyro_calibration_t;

struct qmi8658a_dev_t {
    i2c_master_dev_handle_t i2c_dev;
    bool restore_pending;
    uint8_t awake_ctrl1;
    uint8_t awake_ctrl7;
    uint32_t fresh_count;
    bool fresh_count_valid;
    gyro_calibration_t gyro_calibration;
};

static esp_err_t read_regs(qmi8658a_handle_t handle, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(handle->i2c_dev, &reg, 1, data, len, QMI8658_TIMEOUT_MS);
}

static esp_err_t write_reg(qmi8658a_handle_t handle, uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(handle->i2c_dev, data, sizeof(data), QMI8658_TIMEOUT_MS);
}

esp_err_t qmi8658a_new(i2c_master_bus_handle_t bus, qmi8658a_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(bus && out_handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    qmi8658a_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "no memory");

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658A_I2C_ADDRESS,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &handle->i2c_dev);
    if (ret != ESP_OK) {
        free(handle);
        return ret;
    }

    uint8_t chip_id = 0;
    ret = read_regs(handle, QMI8658_REG_WHO_AM_I, &chip_id, 1);
    if (ret != ESP_OK || chip_id != QMI8658_CHIP_ID) {
        ESP_LOGE(TAG, "unexpected WHO_AM_I 0x%02X", chip_id);
        i2c_master_bus_rm_device(handle->i2c_dev);
        free(handle);
        return ret == ESP_OK ? ESP_ERR_INVALID_RESPONSE : ret;
    }

    /* Auto-increment, accel +/-4 g and gyro +/-512 dps. ODR code 5 is
     * nominally 250 Hz; the effective rate is 224.2 Hz in 6-DoF mode. */
    ESP_GOTO_ON_ERROR(write_reg(handle, QMI8658_REG_CTRL1, 0x40), fail, TAG, "CTRL1");
    /* The IC remains powered across MCU software shutdown/reset. Wake its
     * oscillator before configuration, then stop an earlier sensor session. */
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_GOTO_ON_ERROR(write_reg(handle, QMI8658_REG_CTRL7, 0x00), fail, TAG, "disable retained session");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_GOTO_ON_ERROR(write_reg(handle, QMI8658_REG_CTRL2, 0x15), fail, TAG, "CTRL2");
    ESP_GOTO_ON_ERROR(write_reg(handle, QMI8658_REG_CTRL3, 0x55), fail, TAG, "CTRL3");
    ESP_GOTO_ON_ERROR(write_reg(handle, QMI8658_REG_CTRL5, 0x11), fail, TAG, "CTRL5");
    ESP_GOTO_ON_ERROR(write_reg(handle, QMI8658_REG_CTRL7, 0x03), fail, TAG, "CTRL7");

    ESP_LOGI(TAG, "QMI8658A detected at 0x%02X", QMI8658A_I2C_ADDRESS);
    *out_handle = handle;
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(handle->i2c_dev);
    free(handle);
    return ret;
}

esp_err_t qmi8658a_del(qmi8658a_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    esp_err_t err = i2c_master_bus_rm_device(handle->i2c_dev);
    free(handle);
    return err;
}

static float dot_product(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float distance_squared(const float a[3], const float b[3])
{
    const float difference[] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return dot_product(difference, difference);
}

static void break_stationary_window(gyro_calibration_t *calibration)
{
    calibration->samples = 0;
    calibration->stationary = false;
}

static void update_gyro_bias(gyro_calibration_t *calibration, const float accel[3],
                            const float gyro[3], TickType_t now)
{
    const TickType_t elapsed = now - calibration->last_sample;
    calibration->last_sample = now;
    if (elapsed > pdMS_TO_TICKS(GYRO_STILL_MAX_GAP_MS)) break_stationary_window(calibration);

    const float accel_sq = dot_product(accel, accel);
    const float min_gravity = GRAVITY_MPS2 - GYRO_STILL_GRAVITY_TOL_MPS2;
    const float max_gravity = GRAVITY_MPS2 + GYRO_STILL_GRAVITY_TOL_MPS2;
    if (accel_sq < min_gravity * min_gravity || accel_sq > max_gravity * max_gravity ||
        dot_product(gyro, gyro) > GYRO_STILL_RAW_LIMIT_DPS * GYRO_STILL_RAW_LIMIT_DPS ||
        (calibration->valid && distance_squared(gyro, calibration->bias) >
         GYRO_STILL_RESIDUAL_DPS * GYRO_STILL_RESIDUAL_DPS)) {
        break_stationary_window(calibration);
        return;
    }
    if (!calibration->valid) {
        /* Rotation around gravity leaves the accelerometer unchanged. Bound
         * that component at first acquisition while allowing a larger offset
         * transverse to gravity when the gravity vector remains stable. */
        const float gyro_along_gravity = dot_product(gyro, accel);
        if (gyro_along_gravity * gyro_along_gravity >
            GYRO_STILL_INITIAL_YAW_DPS * GYRO_STILL_INITIAL_YAW_DPS * accel_sq) {
            break_stationary_window(calibration);
            return;
        }
    }

    if (calibration->samples &&
        (distance_squared(accel, calibration->accel_reference) >
         GYRO_STILL_ACCEL_DELTA_MPS2 * GYRO_STILL_ACCEL_DELTA_MPS2 ||
         distance_squared(gyro, calibration->gyro_mean) >
         GYRO_STILL_VARIATION_DPS * GYRO_STILL_VARIATION_DPS)) {
        break_stationary_window(calibration);
    }
    if (!calibration->samples) {
        calibration->window_start = now;
        calibration->samples = 1;
        memcpy(calibration->accel_reference, accel, sizeof(calibration->accel_reference));
        memcpy(calibration->gyro_mean, gyro, sizeof(calibration->gyro_mean));
        return;
    }
    if (!elapsed) return;

    if (!calibration->stationary) {
        const float weight = 1.0f / (float)++calibration->samples;
        for (unsigned axis = 0; axis < 3; ++axis)
            calibration->gyro_mean[axis] += weight * (gyro[axis] - calibration->gyro_mean[axis]);
        if (calibration->samples < GYRO_STILL_MIN_SAMPLES ||
            now - calibration->window_start < pdMS_TO_TICKS(GYRO_STILL_MIN_MS)) return;
        calibration->stationary = true;
        if (!calibration->valid) {
            memcpy(calibration->bias, calibration->gyro_mean, sizeof(calibration->bias));
            calibration->valid = true;
        }
        return;
    }

    /* The raw gyro gate bounds the mean and this convex update. The time
     * constant and maximum allowed sample gap bound the correction step. */
    const float weight = (float)elapsed / (float)(pdMS_TO_TICKS(GYRO_BIAS_TIME_CONSTANT_MS) + elapsed);
    for (unsigned axis = 0; axis < 3; ++axis) {
        calibration->gyro_mean[axis] += weight * (gyro[axis] - calibration->gyro_mean[axis]);
        calibration->bias[axis] += weight * (gyro[axis] - calibration->bias[axis]);
    }
}

static void decode_sample(qmi8658a_handle_t handle, const uint8_t raw[QMI8658_SAMPLE_BYTES],
                          qmi8658a_sample_t *sample)
{
    const float accel_scale = (4.0f * GRAVITY_MPS2) / 32768.0f;
    const float gyro_scale = 512.0f / 32768.0f;
    for (int i = 0; i < 3; ++i) {
        const int16_t a = (int16_t)((uint16_t)raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8));
        const int16_t g = (int16_t)((uint16_t)raw[6 + i * 2] | ((uint16_t)raw[7 + i * 2] << 8));
        sample->accel_mps2[i] = a * accel_scale;
        sample->gyro_raw_dps[i] = g * gyro_scale;
    }
    gyro_calibration_t *calibration = &handle->gyro_calibration;
    update_gyro_bias(calibration, sample->accel_mps2, sample->gyro_raw_dps, xTaskGetTickCount());
    sample->gyro_bias_valid = calibration->valid;
    sample->gyro_stationary = calibration->stationary;
    for (unsigned axis = 0; axis < 3; ++axis) {
        sample->gyro_bias_dps[axis] = calibration->bias[axis];
        sample->gyro_dps[axis] = sample->gyro_raw_dps[axis] - calibration->bias[axis];
    }
}

esp_err_t qmi8658a_read(qmi8658a_handle_t handle, qmi8658a_sample_t *sample)
{
    ESP_RETURN_ON_FALSE(handle && sample, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!handle->restore_pending, ESP_ERR_INVALID_STATE, TAG, "sensor asleep/restoration pending");
    uint8_t raw[QMI8658_SAMPLE_BYTES];
    const esp_err_t error = read_regs(handle, QMI8658_REG_AX_L, raw, sizeof(raw));
    if (error != ESP_OK) {
        break_stationary_window(&handle->gyro_calibration);
        ESP_RETURN_ON_ERROR(error, TAG, "read sample");
    }
    decode_sample(handle, raw, sample);
    return ESP_OK;
}

static uint32_t sample_count(const uint8_t bytes[QMI8658_COUNTER_BYTES])
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16);
}

esp_err_t qmi8658a_read_fresh(qmi8658a_handle_t handle, qmi8658a_sample_t *sample)
{
    ESP_RETURN_ON_FALSE(handle && sample, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!handle->restore_pending, ESP_ERR_INVALID_STATE, TAG, "sensor asleep/restoration pending");

    /* Bracket the output burst with the sample counter to reject crossings.
     * Keeping all reads within the 16-byte LP I2C FIFO avoids a refill. */
    const uint8_t counter_reg = QMI8658_REG_TIMESTAMP, output_reg = QMI8658_REG_AX_L;
    uint8_t before[QMI8658_COUNTER_BYTES];
    uint8_t raw[QMI8658_SAMPLE_BYTES];
    uint8_t after[QMI8658_COUNTER_BYTES];
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        esp_err_t error = i2c_master_transmit_receive(handle->i2c_dev, &counter_reg, 1, before,
                                                      sizeof(before), QMI8658_FAST_TIMEOUT_MS);
        if (error == ESP_OK) {
            const uint32_t count = sample_count(before);
            if (handle->fresh_count_valid && count == handle->fresh_count) return ESP_ERR_NOT_FINISHED;
            error = i2c_master_transmit_receive(handle->i2c_dev, &output_reg, 1, raw, sizeof(raw),
                                                QMI8658_FAST_TIMEOUT_MS);
            if (error == ESP_OK) {
                error = i2c_master_transmit_receive(handle->i2c_dev, &counter_reg, 1, after,
                                                    sizeof(after), QMI8658_FAST_TIMEOUT_MS);
            }
            if (error == ESP_OK && count == sample_count(after)) {
                handle->fresh_count = count;
                handle->fresh_count_valid = true;
                decode_sample(handle, raw, sample);
                return ESP_OK;
            }
        }
        if (error != ESP_OK) {
            break_stationary_window(&handle->gyro_calibration);
            return error;
        }
    }
    return ESP_ERR_NOT_FINISHED;
}

static esp_err_t write_verify(qmi8658a_handle_t handle, uint8_t reg, uint8_t value,
                               uint8_t mask)
{
    ESP_RETURN_ON_ERROR(write_reg(handle, reg, value), TAG, "write power mode");
    uint8_t actual = 0;
    ESP_RETURN_ON_ERROR(read_regs(handle, reg, &actual, 1), TAG, "read power mode");
    return (actual & mask) == (value & mask) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t qmi8658a_set_sleep(qmi8658a_handle_t handle, bool sleep)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    if (sleep) {
        break_stationary_window(&handle->gyro_calibration);
        handle->fresh_count_valid = false;
        if (!handle->restore_pending) {
            uint8_t ctrl1 = 0, ctrl7 = 0;
            ESP_RETURN_ON_ERROR(read_regs(handle, QMI8658_REG_CTRL1, &ctrl1, 1), TAG, "save CTRL1");
            ESP_RETURN_ON_ERROR(read_regs(handle, QMI8658_REG_CTRL7, &ctrl7, 1), TAG, "save CTRL7");
            handle->awake_ctrl1 = ctrl1;
            handle->awake_ctrl7 = ctrl7;
            /* Save before the first write: a NACK may still have changed the IC. */
            handle->restore_pending = true;
        }
        ESP_RETURN_ON_ERROR(write_verify(handle, QMI8658_REG_CTRL7,
            handle->awake_ctrl7 & (uint8_t)~0x13U, 0x13), TAG, "disable sensors");
        /* Rev A section 7.3: sensor-off transition takes up to 2/ODR.
         * This driver's 6-DoF ODR is 224.2 Hz; 20 ms exceeds that interval. */
        vTaskDelay(pdMS_TO_TICKS(20));
        /* Rev A section 7.1/Table31: retained-register Power Down, typical 6 uA.
         * Mask INT enables too, so the powered sensor leaves its IRQs high-Z. */
        return write_verify(handle, QMI8658_REG_CTRL1,
            (handle->awake_ctrl1 & (uint8_t)~0x18U) | 0x01, 0x19);
    }
    if (!handle->restore_pending) return ESP_OK;
    ESP_RETURN_ON_ERROR(write_verify(handle, QMI8658_REG_CTRL1,
        handle->awake_ctrl1, 0xFF), TAG, "restore oscillator/interface");
    /* Rev A 7.3: leaving Power Down needs at most 15 ms before reconfiguration. */
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(write_verify(handle, QMI8658_REG_CTRL7,
        handle->awake_ctrl7, 0xFF), TAG, "restore sensors");
    /* Gyro wake is typically 150 ms plus filter settling; discard old snapshots
     * in telemetry and allow this initial settling interval before new reads. */
    vTaskDelay(pdMS_TO_TICKS(200));
    handle->restore_pending = false;
    return ESP_OK;
}
