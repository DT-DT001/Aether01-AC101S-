#include "tg29.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TG29_REG_DCDC_ENABLE     0x80
#define TG29_REG_DCDC2_VOLTAGE   0x84
#define TG29_REG_LDO_ENABLE_0    0x90
#define TG29_REG_LDO_ENABLE_1    0x91
#define TG29_REG_ALDO1_VOLTAGE   0x93
#define TG29_REG_ALDO2_VOLTAGE   0x94
#define TG29_REG_ALDO3_VOLTAGE   0x95
#define TG29_REG_ALDO4_VOLTAGE   0x96
#define TG29_REG_BLDO1_VOLTAGE   0x97
#define TG29_REG_BLDO2_VOLTAGE   0x98
#define TG29_REG_BLDO3_VOLTAGE   0x99
#define TG29_REG_BLDO4_VOLTAGE   0x9A
#define TG29_REG_CLDO1_VOLTAGE   0x9B
#define TG29_REG_CLDO2_VOLTAGE   0x9C
#define TG29_REG_CLDO3_VOLTAGE   0x9D
#define TG29_REG_CLDO4_VOLTAGE   0x9E
#define TG29_REG_CPUSLDO_VOLTAGE 0x9F
#define TG29_REG_IRQ_ENABLE_1    0x41
#define TG29_REG_IRQ_STATUS_1    0x49
#define TG29_KEY_EVENT_MASK      0x0F
#define TG29_REG_POWER_STATUS    0x00
#define TG29_REG_CHARGE_STATUS   0x01
#define TG29_REG_GAUGE_CONTROL   0x0B
#define TG29_REG_BAT_DETECTION   0x68
#define TG29_REG_BAT_PERCENTAGE  0xA4
#define TG29_REG_ADC_ENABLE      0xC0
#define TG29_REG_VBAT_HIGH       0xC4
#define TG29_REG_VBAT_LOW        0xC5
#define TG29_REG_SOFTWARE_POWER  0x27
#define TG29_REG_RBFET_CONTROL   0x13
#define TG29_REG_MODULE_ENABLE_2 0x19
#define TG29_REG_BOOST_CONFIG    0x1E
#define TG29_REG_PRECHARGE       0x61
#define TG29_REG_FAST_CHARGE     0x62
#define TG29_REG_CC_MODE         0xE3
#define TG29_REG_CC_STATUS       0xE7
#define TG29_BOOST_ENABLE_BIT    0x10U
#define TG29_BOOST_5126MV_500MA  0x90U
#define TG29_MANUAL_VBUS_MAX_MV  800U
#define TG29_MANUAL_ADC_SETTLE_MS 1000U
#define TG29_MANUAL_ADC_BUDGET_MS 2000U

#define TG29_DCDC2_ENABLE_BIT    (1U << 1)
#define TG29_DCDC2_3300MV_CODE   0x69
#define TG29_ALDO2_ENABLE_BIT    (1U << 1)
#define TG29_ALDO2_3300MV_CODE   0x1C
#define TG29_TIMEOUT_MS          100

static const char *TAG = "tg29";

struct tg29_dev_t {
    i2c_master_dev_handle_t i2c_dev;
    SemaphoreHandle_t mutex;
    atomic_bool source_allowed;
    atomic_uint source_epoch;
    atomic_uint authorized_epoch;
    atomic_bool manual_start_active; /* Reserves the unlocked ADC preflight. */
    bool manual_otg;
    bool manual_output_started;
    bool fixed_ts_10k;
    bool ts_bias_owned;
    uint8_t ts_idle_nibble;
    bool vbus_adc_owned;
    atomic_uint input_token; /* Independent retained external ADC owner. */
    uint32_t input_sequence;
    bool input_adc_owned;
    atomic_bool input_probe_active; /* Bringup probe also releases the mutex. */
};

typedef struct {
    uint8_t enable_reg;
    uint8_t enable_bit;
    uint8_t voltage_reg;
} rail_reg_map_t;

static esp_err_t tg29_read_reg(tg29_handle_t handle, uint8_t reg, uint8_t *value)
{
    ESP_RETURN_ON_FALSE(handle && value, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    return i2c_master_transmit_receive(handle->i2c_dev, &reg, 1, value, 1, TG29_TIMEOUT_MS);
}

static esp_err_t tg29_write_reg(tg29_handle_t handle, uint8_t reg, uint8_t value)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(handle->i2c_dev, data, sizeof(data), TG29_TIMEOUT_MS);
}

static esp_err_t tg29_write_checked(tg29_handle_t handle, uint8_t reg,
                                     uint8_t value, uint8_t mask)
{
    ESP_RETURN_ON_ERROR(tg29_write_reg(handle, reg, value), TAG, "write register 0x%02X", reg);
    uint8_t actual = 0;
    ESP_RETURN_ON_ERROR(tg29_read_reg(handle, reg, &actual), TAG, "verify register 0x%02X", reg);
    ESP_RETURN_ON_FALSE((actual & mask) == (value & mask), ESP_ERR_INVALID_RESPONSE,
                        TAG, "register 0x%02X mismatch: wanted 0x%02X, read 0x%02X (mask 0x%02X)",
                        reg, value, actual, mask);
    return ESP_OK;
}

/* Capture fault latches without clearing RW1C bits or changing charging,
 * protection, ADC selection, or DCDC2 voltage. Values are register settings,
 * not measurements of the physical output voltage or temperature. */
static void tg29_log_diagnostics_locked(tg29_handle_t handle, const char *phase)
{
    static const uint8_t registers[] = {
        0x00, 0x01, 0x05, 0x08, 0x0B, 0x13, 0x19, 0x21, 0x22, 0x23, 0x4A, 0x50, 0x62, 0x64, 0x65,
    };
    ESP_LOGI(TAG, "PMIC diagnostic snapshot: %s", phase);
    for (size_t i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i) {
        uint8_t value = 0;
        const esp_err_t err = tg29_read_reg(handle, registers[i], &value);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "diagnostic register 0x%02X unavailable: %s",
                     registers[i], esp_err_to_name(err));
            break;
        }
        ESP_LOGI(TAG, "PMIC reg[0x%02X]=0x%02X", registers[i], value);
    }
}

/* The sensors on ALDO2 share the PMIC's I2C wires. Preserve that rail even on
 * errors: removing its supply can clamp the bus and prevent recovery. Never
 * turn an unknown-voltage ALDO2 on as part of failure cleanup. */
static esp_err_t tg29_disable_optional_rails_locked(tg29_handle_t handle)
{
    esp_err_t first_error = tg29_write_checked(handle, TG29_REG_DCDC_ENABLE,
                                               TG29_DCDC2_ENABLE_BIT, 0x07);
    uint8_t ldo0 = 0;
    esp_err_t err = tg29_read_reg(handle, TG29_REG_LDO_ENABLE_0, &ldo0);
    if (err == ESP_OK) {
        err = tg29_write_checked(handle, TG29_REG_LDO_ENABLE_0,
                                  ldo0 & TG29_ALDO2_ENABLE_BIT, 0xFF);
    } else {
        ESP_LOGE(TAG, "cannot read ALDO2 state; skipping LDO0 shutdown to preserve shared I2C supply");
    }
    if (err != ESP_OK && first_error == ESP_OK) first_error = err;
    err = tg29_write_checked(handle, TG29_REG_LDO_ENABLE_1, 0, 0x1F);
    if (err != ESP_OK && first_error == ESP_OK) first_error = err;
    return first_error;
}

static bool tg29_map_rail(tg29_rail_t rail, rail_reg_map_t *map)
{
    if (!map) {
        return false;
    }
    if (rail >= TG29_RAIL_DCDC1 && rail <= TG29_RAIL_DCDC3) {
        *map = (rail_reg_map_t) {
            .enable_reg = TG29_REG_DCDC_ENABLE,
            .enable_bit = (uint8_t)(1U << (rail - TG29_RAIL_DCDC1)),
            .voltage_reg = (uint8_t)(0x83 + (rail - TG29_RAIL_DCDC1)),
        };
        return true;
    }
    if (rail >= TG29_RAIL_ALDO1 && rail <= TG29_RAIL_ALDO4) {
        *map = (rail_reg_map_t) {
            .enable_reg = TG29_REG_LDO_ENABLE_0,
            .enable_bit = (uint8_t)(1U << (rail - TG29_RAIL_ALDO1)),
            .voltage_reg = (uint8_t)(TG29_REG_ALDO1_VOLTAGE + (rail - TG29_RAIL_ALDO1)),
        };
        return true;
    }
    if (rail >= TG29_RAIL_BLDO1 && rail <= TG29_RAIL_BLDO4) {
        *map = (rail_reg_map_t) {
            .enable_reg = TG29_REG_LDO_ENABLE_0,
            .enable_bit = (uint8_t)(1U << (4 + rail - TG29_RAIL_BLDO1)),
            .voltage_reg = (uint8_t)(TG29_REG_BLDO1_VOLTAGE + (rail - TG29_RAIL_BLDO1)),
        };
        return true;
    }
    if (rail >= TG29_RAIL_CLDO1 && rail <= TG29_RAIL_CLDO4) {
        *map = (rail_reg_map_t) {
            .enable_reg = TG29_REG_LDO_ENABLE_1,
            .enable_bit = (uint8_t)(1U << (rail - TG29_RAIL_CLDO1)),
            .voltage_reg = (uint8_t)(TG29_REG_CLDO1_VOLTAGE + (rail - TG29_RAIL_CLDO1)),
        };
        return true;
    }
    if (rail == TG29_RAIL_CPUSLDO) {
        *map = (rail_reg_map_t) {
            .enable_reg = TG29_REG_LDO_ENABLE_1,
            .enable_bit = (1U << 4),
            .voltage_reg = TG29_REG_CPUSLDO_VOLTAGE,
        };
        return true;
    }
    return false;
}

esp_err_t tg29_new(i2c_master_bus_handle_t bus, tg29_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(bus && out_handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    tg29_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "no memory");
    atomic_init(&handle->source_allowed, false);
    atomic_init(&handle->source_epoch, 0);
    atomic_init(&handle->manual_start_active, false);
    atomic_init(&handle->authorized_epoch, 0);
    atomic_init(&handle->input_token, 0);
    atomic_init(&handle->input_probe_active, false);
    handle->mutex = xSemaphoreCreateRecursiveMutex();
    if (!handle->mutex) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TG29_I2C_ADDRESS_7BIT,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &handle->i2c_dev);
    if (err != ESP_OK) {
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return err;
    }

    uint8_t status = 0;
    err = tg29_read_reg(handle, 0x00, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TG29 register read failed at 7-bit address 0x%02X, 100 kHz: %s",
                 TG29_I2C_ADDRESS_7BIT, esp_err_to_name(err));
        i2c_master_bus_rm_device(handle->i2c_dev);
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return err;
    }
    ESP_LOGI(TAG, "TG29 detected at 7-bit address 0x%02X, status=0x%02X",
             TG29_I2C_ADDRESS_7BIT, status);
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t tg29_del(tg29_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    esp_err_t err = i2c_master_bus_rm_device(handle->i2c_dev);
    if (err != ESP_OK) return err;
    vSemaphoreDelete(handle->mutex);
    free(handle);
    return err;
}

static esp_err_t set_ldo_voltage_locked(tg29_handle_t handle, tg29_rail_t rail, uint16_t millivolts)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    ESP_RETURN_ON_FALSE(rail >= TG29_RAIL_ALDO1 && rail <= TG29_RAIL_CLDO4,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "only ALDO/BLDO/CLDO support this voltage encoding");
    ESP_RETURN_ON_FALSE(rail != TG29_RAIL_ALDO2 || millivolts == 3300,
                        ESP_ERR_INVALID_ARG, TAG, "ALDO2 shared I2C supply must remain at 3300 mV");
    ESP_RETURN_ON_FALSE(millivolts >= 500 && millivolts <= 3500 &&
                        ((millivolts - 500) % 100) == 0,
                        ESP_ERR_INVALID_ARG, TAG, "LDO voltage must be 500..3500 mV in 100 mV steps");

    rail_reg_map_t map;
    ESP_RETURN_ON_FALSE(tg29_map_rail(rail, &map), ESP_ERR_INVALID_ARG, TAG, "unknown rail");
    const uint8_t code = (uint8_t)((millivolts - 500) / 100);
    uint8_t old_value = 0;
    ESP_RETURN_ON_ERROR(tg29_read_reg(handle, map.voltage_reg, &old_value), TAG, "read voltage reg");
    return tg29_write_checked(handle, map.voltage_reg,
                              (uint8_t)((old_value & 0xE0) | code), 0x1F);
}

static esp_err_t set_rail_enabled_locked(tg29_handle_t handle, tg29_rail_t rail, bool enabled)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    ESP_RETURN_ON_FALSE(!(rail == TG29_RAIL_DCDC2 && !enabled), ESP_ERR_INVALID_ARG, TAG,
                        "DCDC2 powers the MCU and cannot be disabled at runtime");
    ESP_RETURN_ON_FALSE(!(rail == TG29_RAIL_ALDO2 && !enabled), ESP_ERR_INVALID_ARG, TAG,
                        "ALDO2 powers shared I2C sensors and cannot be disabled at runtime");
    if (rail == TG29_RAIL_ALDO2) {
        uint8_t voltage = 0;
        ESP_RETURN_ON_ERROR(tg29_read_reg(handle, TG29_REG_ALDO2_VOLTAGE, &voltage),
                            TAG, "read shared I2C supply voltage");
        ESP_RETURN_ON_FALSE((voltage & 0x1F) == TG29_ALDO2_3300MV_CODE,
                            ESP_ERR_INVALID_STATE, TAG, "verify ALDO2 3300 mV before enabling shared I2C supply");
    }

    rail_reg_map_t map;
    ESP_RETURN_ON_FALSE(tg29_map_rail(rail, &map), ESP_ERR_INVALID_ARG, TAG, "unknown rail");
    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(tg29_read_reg(handle, map.enable_reg, &value), TAG, "read enable reg");
    value = enabled ? (uint8_t)(value | map.enable_bit) : (uint8_t)(value & ~map.enable_bit);
    return tg29_write_checked(handle, map.enable_reg, value, map.enable_bit);
}

static uint16_t tg29_decode_dcdc2_mv(uint8_t value)
{
    const uint8_t code = value & 0x7F;
    if (code <= 70) {
        return (uint16_t)(500 + code * 10);
    }
    if (code <= 87) {
        return (uint16_t)(1220 + (code - 71) * 20);
    }
    if (code <= 106) {
        return (uint16_t)(1600 + (code - 88) * 100);
    }
    return 0;
}

static esp_err_t get_power_state_locked(tg29_handle_t handle, tg29_power_state_t *state)
{
    ESP_RETURN_ON_FALSE(handle && state, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    tg29_power_state_t snapshot = {0};
    uint8_t dcdc2_voltage = 0;
    ESP_RETURN_ON_ERROR(tg29_read_reg(handle, TG29_REG_DCDC_ENABLE, &snapshot.dcdc_enable), TAG, "read DCDC enables");
    ESP_RETURN_ON_ERROR(tg29_read_reg(handle, TG29_REG_LDO_ENABLE_0, &snapshot.ldo_enable_0), TAG, "read LDO enables 0");
    ESP_RETURN_ON_ERROR(tg29_read_reg(handle, TG29_REG_LDO_ENABLE_1, &snapshot.ldo_enable_1), TAG, "read LDO enables 1");
    ESP_RETURN_ON_ERROR(tg29_read_reg(handle, TG29_REG_DCDC2_VOLTAGE, &dcdc2_voltage), TAG, "read DCDC2 voltage");
    snapshot.dcdc2_mv = tg29_decode_dcdc2_mv(dcdc2_voltage);
    *state = snapshot;
    return ESP_OK;
}

static esp_err_t apply_power_profile_locked(tg29_handle_t handle, const tg29_power_profile_t *profile)
{
    ESP_RETURN_ON_FALSE(handle && profile, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(profile->enable_sensor_rail && profile->aldo2_mv == 3300,
                        ESP_ERR_INVALID_ARG, TAG, "board requires ALDO2 shared I2C supply enabled at 3300 mV");

    /* Reject a bad profile before changing any rail; otherwise a later invalid
     * voltage could leave earlier rails partially reprogrammed. */
    const uint16_t voltages[] = {profile->aldo2_mv, profile->aldo4_mv,
                                 profile->bldo1_mv, profile->cldo1_mv, profile->cldo3_mv};
    for (size_t i = 0; i < sizeof(voltages) / sizeof(voltages[0]); ++i) {
        ESP_RETURN_ON_FALSE(voltages[i] >= 500 && voltages[i] <= 3500 &&
                             (voltages[i] - 500) % 100 == 0,
                             ESP_ERR_INVALID_ARG, TAG, "invalid profile voltage");
    }

    /* Configure the shared-bus sensor supply first. Unlike other peripheral
     * loads, ALDO2 must stay on while communicating with this PMIC. */
    esp_err_t err = set_ldo_voltage_locked(handle, TG29_RAIL_ALDO2, profile->aldo2_mv);
    if (err != ESP_OK) goto failed;
    err = set_rail_enabled_locked(handle, TG29_RAIL_ALDO2, true);
    if (err != ESP_OK) goto failed;

    /* MCU resets do not necessarily reset TG29. Remove inherited optional
     * loads before changing their voltage, retaining DCDC2 and ALDO2. */
    err = tg29_disable_optional_rails_locked(handle);
    if (err != ESP_OK) goto failed;

    /* DCDC2 is never reprogrammed by this driver. */
    uint8_t dcdc2_voltage = 0;
    err = tg29_read_reg(handle, TG29_REG_DCDC2_VOLTAGE, &dcdc2_voltage);
    if (err != ESP_OK) goto failed;
    if ((dcdc2_voltage & 0x7F) != TG29_DCDC2_3300MV_CODE) {
        ESP_LOGE(TAG, "DCDC2 register=0x%02X (%u mV decoded), expected board 3300 mV setting; verify physically",
                 dcdc2_voltage, tg29_decode_dcdc2_mv(dcdc2_voltage));
        err = ESP_ERR_INVALID_STATE;
        goto failed;
    }

    /* Verify remaining voltage settings before enabling optional loads. */
    const struct {
        tg29_rail_t rail;
        uint16_t mv;
    } voltages_to_set[] = {
        {TG29_RAIL_ALDO4, profile->aldo4_mv},
        {TG29_RAIL_BLDO1, profile->bldo1_mv},
        {TG29_RAIL_CLDO1, profile->cldo1_mv},
        {TG29_RAIL_CLDO3, profile->cldo3_mv},
    };
    for (size_t i = 0; i < sizeof(voltages_to_set) / sizeof(voltages_to_set[0]); ++i) {
        err = set_ldo_voltage_locked(handle, voltages_to_set[i].rail, voltages_to_set[i].mv);
        if (err != ESP_OK) goto failed;
    }

    /* ALDO1/3 and BLDO2/3/4 are unused. BLDO1 is only enabled on SD demand. */
    uint8_t ldo0 = TG29_ALDO2_ENABLE_BIT;
    if (profile->enable_codec_rail) {
        ldo0 |= (1U << 3); /* ALDO4 */
    }
    if (profile->enable_sd_rail) {
        ldo0 |= (1U << 4); /* BLDO1 */
    }
    err = tg29_write_checked(handle, TG29_REG_LDO_ENABLE_0, ldo0, 0xFF);
    if (err != ESP_OK) goto failed;

    /* CLDO2/4 and CPUSLDO are unused. */
    uint8_t ldo1 = 0;
    if (profile->enable_touch_rail) {
        ldo1 |= (1U << 0); /* CLDO1 */
    }
    if (profile->enable_display_rail) {
        ldo1 |= (1U << 2); /* CLDO3 */
    }
    err = tg29_write_checked(handle, TG29_REG_LDO_ENABLE_1, ldo1, 0x1F);
    if (err != ESP_OK) goto failed;

    tg29_power_state_t state;
    err = get_power_state_locked(handle, &state);
    if (err != ESP_OK) goto failed;
    if ((state.dcdc_enable & 0x07) != TG29_DCDC2_ENABLE_BIT ||
        state.ldo_enable_0 != ldo0 || (state.ldo_enable_1 & 0x1F) != ldo1 ||
        state.dcdc2_mv != 3300) {
        ESP_LOGE(TAG, "power rail readback mismatch");
        err = ESP_ERR_INVALID_RESPONSE;
        goto failed;
    }

    ESP_LOGI(TAG, "rails: DCDC=0x%02X LDO0=0x%02X LDO1=0x%02X DCDC2=%u mV",
             state.dcdc_enable, state.ldo_enable_0, state.ldo_enable_1, state.dcdc2_mv);
    return ESP_OK;

failed:
    tg29_log_diagnostics_locked(handle, "power policy failed; shutting down peripheral rails");
    const esp_err_t cleanup_err = tg29_disable_optional_rails_locked(handle);
    if (cleanup_err != ESP_OK) {
        ESP_LOGE(TAG, "peripheral rail shutdown could not be verified: %s",
                 esp_err_to_name(cleanup_err));
    }
    return err;
}

/* Keep register read/modify/write and profile application atomic across callers. */
esp_err_t tg29_set_ldo_voltage(tg29_handle_t handle, tg29_rail_t rail, uint16_t millivolts)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = set_ldo_voltage_locked(handle, rail, millivolts);
    xSemaphoreGiveRecursive(handle->mutex);
    return err;
}

esp_err_t tg29_set_rail_enabled(tg29_handle_t handle, tg29_rail_t rail, bool enabled)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = set_rail_enabled_locked(handle, rail, enabled);
    xSemaphoreGiveRecursive(handle->mutex);
    return err;
}

esp_err_t tg29_get_power_state(tg29_handle_t handle, tg29_power_state_t *state)
{
    if (!handle || !state) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = get_power_state_locked(handle, state);
    xSemaphoreGiveRecursive(handle->mutex);
    return err;
}

esp_err_t tg29_get_battery_state(tg29_handle_t handle, tg29_battery_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    memset(state, 0, sizeof(*state));
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    /* A local snapshot is published only when every required read succeeds.
     * Do not enable disabled measurements or acknowledge any interrupt. */
    static const uint8_t control_regs[] = {
        TG29_REG_POWER_STATUS, TG29_REG_CHARGE_STATUS, TG29_REG_BAT_DETECTION,
        TG29_REG_GAUGE_CONTROL, TG29_REG_ADC_ENABLE,
    };
    uint8_t control[sizeof(control_regs)] = {0};
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(control_regs); ++i) {
        err = tg29_read_reg(handle, control_regs[i], &control[i]);
        if (err != ESP_OK) goto done;
    }
    tg29_battery_state_t snapshot = {
        .presence_valid = (control[2] & 0x01U) != 0,
        .battery_present = (control[0] & 0x08U) != 0,
        .vbus_present = (control[0] & 0x20U) != 0,
        .charge_state = control[1] & 0x07U,
        .current_direction = (control[1] >> 5) & 0x03U,
    };
    snapshot.charge_state_valid = snapshot.charge_state <= 5U && snapshot.current_direction <= 2U;
    const bool battery = snapshot.presence_valid && snapshot.battery_present;
    const bool charger = battery && snapshot.vbus_present && snapshot.charge_state_valid;
    snapshot.charging = charger && snapshot.current_direction == 1U && snapshot.charge_state <= 3U;
    snapshot.charge_done = charger && snapshot.charge_state == 4U;
    if (battery && (control[3] & 0x04U)) {
        err = tg29_read_reg(handle, TG29_REG_BAT_PERCENTAGE, &snapshot.percentage);
        if (err != ESP_OK) goto done;
        snapshot.percentage_valid = snapshot.percentage <= 100U;
    }
    if (battery && (control[4] & 0x01U)) {
        uint8_t high = 0, low = 0;
        /* Reading the high register first latches the matching low byte. */
        err = tg29_read_reg(handle, TG29_REG_VBAT_HIGH, &high);
        if (err != ESP_OK) goto done;
        err = tg29_read_reg(handle, TG29_REG_VBAT_LOW, &low);
        if (err != ESP_OK) goto done;
        snapshot.voltage_mv = (uint16_t)(((uint16_t)(high & 0x3FU) << 8) | low);
        /* The manual defines no explicit ADC-valid bit. Preserve raw data;
         * reject only these conservative endpoints, not a guessed cell range. */
        snapshot.voltage_valid = snapshot.voltage_mv != 0 && snapshot.voltage_mv != 0x3FFFU;
    }
    *state = snapshot;
done:
    xSemaphoreGiveRecursive(handle->mutex);
    return err;
}

esp_err_t tg29_configure_key_wakeup(tg29_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    /* REG49 is RW1C. A read/modify/write of the full byte would acknowledge
     * unrelated VBUS/BAT events. Clear only old PWRON key latches at startup,
     * and leave every protection/charge interrupt and enable untouched. */
    uint8_t pending = 0, enables = 0;
    esp_err_t err = tg29_read_reg(handle, TG29_REG_IRQ_STATUS_1, &pending);
    if (err == ESP_OK && (pending & TG29_KEY_EVENT_MASK))
        err = tg29_write_reg(handle, TG29_REG_IRQ_STATUS_1, pending & TG29_KEY_EVENT_MASK);
    if (err == ESP_OK) err = tg29_read_reg(handle, TG29_REG_IRQ_ENABLE_1, &enables);
    if (err == ESP_OK)
        err = tg29_write_checked(handle, TG29_REG_IRQ_ENABLE_1,
                                  enables | TG29_KEY_EVENT_MASK, 0xFF);
    xSemaphoreGiveRecursive(handle->mutex);
    return err;
}

esp_err_t tg29_get_key_events(tg29_handle_t handle, uint8_t *events)
{
    if (!events) return ESP_ERR_INVALID_ARG;
    *events = 0;
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    uint8_t pending = 0;
    esp_err_t err = tg29_read_reg(handle, TG29_REG_IRQ_STATUS_1, &pending);
    if (err == ESP_OK) {
        *events = pending & TG29_KEY_EVENT_MASK;
        /* Do not read back RW1C bits: a new edge may arrive immediately after
         * acknowledgement and must remain pending for the next poll. A failed
         * acknowledgement still returns the observed events to the caller. */
        if (*events) err = tg29_write_reg(handle, TG29_REG_IRQ_STATUS_1, *events);
    }
    xSemaphoreGiveRecursive(handle->mutex);
    return err;
}

esp_err_t tg29_request_power_off(tg29_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    /* Final shutdown must not wait for the ordinary one-second PMIC lock. */
    if (xSemaphoreTakeRecursive(handle->mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    uint8_t control = 0;
    esp_err_t err = tg29_read_reg(handle, TG29_REG_SOFTWARE_POWER, &control);
    if (err == ESP_OK) {
        /* TG29 v0.9 p34 REG27: bit1 restart and bit0 off are RWAC commands.
         * Never replay a restart, nor change bit2 (16s forced-off) or bit3. */
        if (control & 0x02U) err = ESP_ERR_INVALID_STATE;
        else err = tg29_write_reg(handle, TG29_REG_SOFTWARE_POWER,
                                  (control & (uint8_t)~0x03U) | 0x01U);
    }
    xSemaphoreGiveRecursive(handle->mutex);
    return err;
}

esp_err_t tg29_apply_power_profile(tg29_handle_t handle, const tg29_power_profile_t *profile)
{
    if (!handle || !profile) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = apply_power_profile_locked(handle, profile);
    xSemaphoreGiveRecursive(handle->mutex);
    return err;
}

static bool source_permission_current(tg29_handle_t h)
{
    /* A cancelled configure may transiently publish its bool; its old epoch
     * can never authorize hardware or a snapshot after GUI OFF. */
    return atomic_load(&h->source_allowed) &&
           atomic_load(&h->authorized_epoch) == atomic_load(&h->source_epoch);
}

void tg29_revoke_otg_source_permission(tg29_handle_t h)
{
    if (h) { atomic_store(&h->source_allowed, false); atomic_fetch_add(&h->source_epoch, 1); }
}

esp_err_t tg29_set_otg_fixed_ts_10k_policy(tg29_handle_t h, bool enabled)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (atomic_load(&h->source_allowed) || h->ts_bias_owned || h->vbus_adc_owned ||
        atomic_load(&h->input_token))
        err = ESP_ERR_INVALID_STATE;
    else h->fixed_ts_10k = enabled;
    xSemaphoreGiveRecursive(h->mutex);
    return err;
}

static esp_err_t restore_owned_vbus_adc_locked(tg29_handle_t h)
{
    if (!h->vbus_adc_owned) return ESP_OK;
    uint8_t adc = 0;
    esp_err_t err = tg29_read_reg(h, TG29_REG_ADC_ENABLE, &adc);
    if (err == ESP_OK && (adc & 0x04U))
        err = tg29_write_checked(h, TG29_REG_ADC_ENABLE, adc & (uint8_t)~0x04U, 0x04U);
    if (err == ESP_OK) h->vbus_adc_owned = false;
    return err;
}

/* Only explicit fixed10k boards may recognize our known warm-reset pattern.
 * Never infer an NTC/external-voltage topology from REG50 alone. */
static esp_err_t restore_otg_measurements_locked(tg29_handle_t h)
{
    uint8_t ts = 0;
    esp_err_t err = ESP_OK;
    if (h->ts_bias_owned || h->fixed_ts_10k) {
        err = tg29_read_reg(h, 0x50, &ts);
        if (err != ESP_OK) return err;
        if (!h->ts_bias_owned && (ts & 0x1FU) == 0x1EU) {
            h->ts_bias_owned = true;
            h->ts_idle_nibble = 0x02U; /* Explicit board's known idle baseline. */
        }
        if (h->ts_bias_owned) {
            if (!(ts & 0x10U) || ((ts & 0x0FU) != 0x0EU &&
                                   (ts & 0x0FU) != h->ts_idle_nibble))
                return ESP_ERR_INVALID_STATE; /* Another owner changed TS; do not overwrite. */
            if ((ts & 0x0FU) != h->ts_idle_nibble)
                err = tg29_write_checked(h, 0x50, (ts & 0xF0U) | h->ts_idle_nibble, 0x0FU);
            if (err != ESP_OK) return err;
            h->ts_bias_owned = false;
        }
    }
    return restore_owned_vbus_adc_locked(h);
}

static esp_err_t prepare_fixed_ts_locked(tg29_handle_t h)
{
    if (!h->fixed_ts_10k) return ESP_OK;
    uint8_t ts = 0, low = 0, high = 0;
    esp_err_t err = tg29_read_reg(h, 0x50, &ts);
    if (err != ESP_OK || !(ts & 0x10U)) return err; /* Real NTC configuration unchanged. */
    if (h->ts_bias_owned) return (ts & 0x1FU) == 0x1EU ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (ts & 0x0CU) return ESP_OK; /* Existing active bias has its own owner. */
    err = tg29_read_reg(h, 0x56, &low);
    if (err == ESP_OK) err = tg29_read_reg(h, 0x57, &high);
    if (err != ESP_OK) return err;
    if (500U < (unsigned)high * 2U || 500U > (unsigned)low * 32U)
        return ESP_ERR_INVALID_STATE; /* Do not widen work protection. */
    h->ts_idle_nibble = ts & 0x0FU;
    h->ts_bias_owned = true; /* An error/ACK may still have changed hardware. */
    return tg29_write_checked(h, 0x50, (ts & 0xF0U) | 0x0EU, 0x1FU);
}

static esp_err_t clear_output_bit_locked(tg29_handle_t h, uint8_t reg, uint8_t mask)
{
    uint8_t control = 0;
    esp_err_t err = tg29_read_reg(h, reg, &control);
    if (err != ESP_OK) return err;
    if (!(control & mask)) return ESP_OK;
    return tg29_write_checked(h, reg, control & (uint8_t)~mask, mask);
}

static esp_err_t boost_off_locked(tg29_handle_t h)
{
    h->manual_output_started = false;
    /* Public TG29 datasheet p24 sec6.9: VBUS needs RBFET13[0] as well as
     * BOOST19[4]. Attempt both OFF independently, even if one bus/write/readback
     * fails. Bias/ADC restoration is safe only after BOTH OFF readbacks pass. */
    const esp_err_t gate = clear_output_bit_locked(h, TG29_REG_RBFET_CONTROL, 0x01U);
    const esp_err_t boost = clear_output_bit_locked(h, TG29_REG_MODULE_ENABLE_2, TG29_BOOST_ENABLE_BIT);
    if (gate != ESP_OK) return gate;
    if (boost != ESP_OK) return boost;
    return restore_otg_measurements_locked(h);
}

static bool otg_cc_state_ready(bool manual, uint8_t cc)
{
    /* A disabled CC FSM can retain any historical E7 state. In explicit
     * manual mode its state is diagnostic only; detector/port OFF, software
     * permission and independent physical power checks remain mandatory. */
    return manual || cc == TG29_CC_ATTACH_SRC;
}

static bool otg_cc_role_ready(bool manual, uint8_t detect, uint8_t mode, uint8_t cc)
{
    if (manual) return !(detect & 0x08U) && mode == TG29_CC_DISABLED;
    return (detect & 0x08U) && (mode == TG29_CC_SOURCE || mode == TG29_CC_DRP) &&
           otg_cc_state_ready(false, cc);
}

/* A failure probe must never delay a newly requested OFF with more I2C.
 * The pure public snapshot passes NULL and retains its existing semantics. */
static esp_err_t probe_read_locked(tg29_handle_t h, uint8_t reg, uint8_t *value,
                                    const unsigned *epoch)
{
    if (epoch && (!source_permission_current(h) || atomic_load(&h->source_epoch) != *epoch))
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = tg29_read_reg(h, reg, value);
    if (err != ESP_OK || !epoch) return err;
    if (!source_permission_current(h) || atomic_load(&h->source_epoch) != *epoch)
        return ESP_ERR_INVALID_STATE;
    if ((reg == TG29_REG_CC_STATUS && !otg_cc_state_ready(h->manual_otg, *value & 15U)) ||
        (reg == TG29_REG_MODULE_ENABLE_2 && !(*value & TG29_BOOST_ENABLE_BIT)) ||
        (reg == TG29_REG_RBFET_CONTROL && !(*value & 0x01U)) ||
        (reg == TG29_REG_GAUGE_CONTROL &&
            (h->manual_otg ? (*value & 0x08U) != 0 : !(*value & 0x08U))) ||
        (reg == TG29_REG_CC_MODE && (h->manual_otg ? (*value & 3U) != TG29_CC_DISABLED :
            ((*value & 3U) != TG29_CC_SOURCE && (*value & 3U) != TG29_CC_DRP))))
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

static esp_err_t read_usb_power_locked(tg29_handle_t h, tg29_usb_power_state_t *state,
                                        const unsigned *epoch)
{
    const uint8_t registers[] = {TG29_REG_CC_MODE, TG29_REG_CC_STATUS,
        TG29_REG_MODULE_ENABLE_2, TG29_REG_BOOST_CONFIG, TG29_REG_POWER_STATUS, TG29_REG_GAUGE_CONTROL, TG29_REG_ADC_ENABLE, TG29_REG_RBFET_CONTROL};
    uint8_t values[sizeof(registers)] = {0};
    for (size_t i = 0; i < sizeof(registers); ++i) {
        const esp_err_t err = probe_read_locked(h, registers[i], &values[i], epoch);
        if (err != ESP_OK) return err;
    }
    static const uint16_t boost_limits[] = {500, 900, 1500, 0};
    const tg29_cc_mode_t mode = (tg29_cc_mode_t)(values[0] & 3U);
    const tg29_cc_state_t cc = (tg29_cc_state_t)(values[1] & 15U);
    *state = (tg29_usb_power_state_t){
        .snapshot_valid = true, .cc_mode = mode, .cc_state = cc, .cc_raw = values[1],
        .cc_detection_raw = values[5], .cc_mode_raw = values[0],
        .cc_detection_enabled = (values[5] & 0x08U) != 0,
        .source_allowed = source_permission_current(h),
        .attached_as_source = (values[5] & 0x08U) && cc == TG29_CC_ATTACH_SRC &&
            (mode == TG29_CC_SOURCE || mode == TG29_CC_DRP),
        .attached_as_sink = (values[5] & 0x08U) && cc == TG29_CC_ATTACH_SNK &&
            (mode == TG29_CC_SINK || mode == TG29_CC_DRP),
        .otg_role_ready = otg_cc_role_ready(h->manual_otg, values[5], mode, cc),
        .boost_enabled = (values[2] & TG29_BOOST_ENABLE_BIT) != 0,
        .vbus_good = (values[4] & 0x20U) != 0,
        .module_enable_raw = values[2], .adc_enable_raw = values[6],
        .rbfet_control_raw = values[7], .rbfet_enabled = (values[7] & 0x01U) != 0,
        .boost_mv = (uint16_t)(4550U + 64U * (values[3] >> 4)),
        .boost_limit_ma = boost_limits[values[3] & 3U],
    };
    if (values[6] & 0x04U) {
        uint8_t high = 0, low = 0;
        esp_err_t err = probe_read_locked(h, 0xC6, &high, epoch);
        if (err == ESP_OK) err = probe_read_locked(h, 0xC7, &low, epoch);
        if (err != ESP_OK) { memset(state, 0, sizeof(*state)); return err; }
        const uint16_t raw = (uint16_t)((high & 0x3FU) << 8) | low;
        state->vbus_raw = raw;
        state->vbus_adc_high_raw = high; state->vbus_adc_low_raw = low;
        state->vbus_voltage_valid = raw > 0 && raw <= 0x1FFFU;
        if (state->vbus_voltage_valid) state->vbus_mv = raw;
    }
    return ESP_OK;
}

esp_err_t tg29_get_usb_power_state(tg29_handle_t h, tg29_usb_power_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    memset(state, 0, sizeof(*state));
    if (!h) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    tg29_usb_power_state_t snapshot = {0};
    const esp_err_t err = read_usb_power_locked(h, &snapshot, NULL);
    if (err == ESP_OK) *state = snapshot;
    xSemaphoreGiveRecursive(h->mutex);
    return err;
}

static bool external_input_role_safe(const tg29_usb_power_state_t *p)
{
    return p->snapshot_valid && !p->source_allowed && !p->boost_enabled && !p->rbfet_enabled &&
        (p->cc_mode == TG29_CC_SINK || p->cc_mode == TG29_CC_DRP) &&
        (p->cc_state == TG29_CC_UNATTACH_SNK || p->cc_state == TG29_CC_ATTACHWAIT_SNK ||
         p->cc_state == TG29_CC_ATTACH_SNK ||
         (!p->cc_detection_enabled && p->cc_state == TG29_CC_STATE_DISABLED));
}

static esp_err_t input_release_locked(tg29_handle_t h)
{
    esp_err_t err = ESP_OK;
    if (h->input_adc_owned) {
        uint8_t adc = 0;
        err = tg29_read_reg(h, TG29_REG_ADC_ENABLE, &adc);
        if (err == ESP_OK && (adc & 0x04U))
            err = tg29_write_checked(h, TG29_REG_ADC_ENABLE, adc & (uint8_t)~0x04U, 0x04U);
    }
    if (err == ESP_OK) {
        h->input_adc_owned = false;
        atomic_store(&h->input_token, 0);
    }
    return err;
}

esp_err_t tg29_vbus_input_acquire(tg29_handle_t h, uint32_t *token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    *token = 0;
    if (!h) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    tg29_usb_power_state_t p = {0};
    if (atomic_load(&h->input_token) || atomic_load(&h->manual_start_active) ||
        atomic_load(&h->input_probe_active) || h->vbus_adc_owned || h->ts_bias_owned ||
        source_permission_current(h)) goto done;
    err = read_usb_power_locked(h, &p, NULL);
    if (err != ESP_OK) goto done;
    /* Absence of input is not permission to output. It only hands admission
     * to the separate explicit manual policy, which still proves low VBUS.
     * A disabled/stale CC FSM must not trap a healthy battery-only restart. */
    if (!p.snapshot_valid || p.source_allowed || p.boost_enabled || p.rbfet_enabled) {
        err = ESP_ERR_INVALID_STATE; goto done;
    }
    if (!p.vbus_good) { err = ESP_ERR_NOT_FOUND; goto done; }
    if (!external_input_role_safe(&p)) { err = ESP_ERR_INVALID_STATE; goto done; }
    ++h->input_sequence;
    if (!h->input_sequence) ++h->input_sequence;
    atomic_store(&h->input_token, h->input_sequence);
    *token = h->input_sequence;
    if (!(p.adc_enable_raw & 0x04U)) {
        h->input_adc_owned = true; /* A failed ACK may still have changed C0. */
        err = tg29_write_checked(h, TG29_REG_ADC_ENABLE, p.adc_enable_raw | 0x04U, 0x04U);
        if (err != ESP_OK && input_release_locked(h) == ESP_OK) *token = 0;
    }
done:
    xSemaphoreGiveRecursive(h->mutex);
    return err;
}

esp_err_t tg29_vbus_input_read(tg29_handle_t h, uint32_t token, tg29_usb_power_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    memset(state, 0, sizeof(*state));
    if (!h || !token) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    tg29_usb_power_state_t p = {0};
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (atomic_load(&h->input_token) == token) {
        err = read_usb_power_locked(h, &p, NULL);
        if (err == ESP_OK && (!external_input_role_safe(&p) || !p.vbus_good ||
                             !(p.adc_enable_raw & 0x04U))) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) *state = p;
    }
    xSemaphoreGiveRecursive(h->mutex);
    return err;
}

esp_err_t tg29_vbus_input_release(tg29_handle_t h, uint32_t token)
{
    if (!h || !token) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    const esp_err_t err = atomic_load(&h->input_token) == token ?
                          input_release_locked(h) : ESP_ERR_INVALID_STATE;
    xSemaphoreGiveRecursive(h->mutex);
    return err;
}

static esp_err_t configure_otg_policy(tg29_handle_t h, bool allow_source, bool manual)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    /* Capture before changing permission: a later GUI OFF must cancel this
     * configuration even while I2C or the PMIC mutex is in flight. */
    const unsigned epoch = atomic_load(&h->source_epoch);
    if (allow_source && (atomic_load(&h->manual_start_active) || atomic_load(&h->input_token)))
        return ESP_ERR_INVALID_STATE;
    if (allow_source) atomic_store(&h->source_allowed, false);
    else tg29_revoke_otg_source_permission(h);
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (allow_source && (atomic_load(&h->manual_start_active) || atomic_load(&h->input_token))) {
        xSemaphoreGiveRecursive(h->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t mode = 0, detect = 0;
    esp_err_t err = tg29_read_reg(h, TG29_REG_GAUGE_CONTROL, &detect);
    if (err == ESP_OK) err = tg29_read_reg(h, TG29_REG_CC_MODE, &mode);
    const uint8_t setting = allow_source ? (manual ? 0x10U : 0x13U) : 0x11U;
    if (err == ESP_OK && (!allow_source || manual || h->manual_otg ||
                          (mode & 0x3FU) != setting || !(detect & 0x08U)))
        err = boost_off_locked(h);
    if (allow_source && manual) {
        /* Explicit CC-less output owns software policy, not the hardware CC
         * FSM. Disable both detector and port after checked output OFF; keep
         * them disabled throughout this session. E7 is diagnostic only. */
        if (err == ESP_OK && atomic_load(&h->source_epoch) != epoch) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK)
            err = tg29_write_checked(h, TG29_REG_GAUGE_CONTROL, detect & (uint8_t)~0x09U, 0x08U);
        if (err == ESP_OK && atomic_load(&h->source_epoch) != epoch) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK)
            err = tg29_write_checked(h, TG29_REG_CC_MODE, (mode & 0xC0U) | setting, 0x3FU);
    } else {
        /* After CC-less output, install SINK/DRP while detection is still OFF,
         * then enable the detector against its final verified port policy. */
        if (err == ESP_OK && (mode & 0x3FU) != setting)
            err = tg29_write_checked(h, TG29_REG_CC_MODE,
                                     (mode & 0xC0U) | setting, 0x3FU);
        if (err == ESP_OK && !(detect & 0x08U))
            /* REG0B.bit0 is RWAC; never replay a transient watchdog command. */
            err = tg29_write_checked(h, TG29_REG_GAUGE_CONTROL,
                                     (detect & (uint8_t)~0x01U) | 0x08U, 0x08U);
    }
    if (err == ESP_OK && allow_source && atomic_load(&h->source_epoch) != epoch)
        err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && allow_source) {
        h->manual_otg = manual;
        atomic_store(&h->authorized_epoch, epoch);
        atomic_store(&h->source_allowed, true);
        if (!source_permission_current(h)) err = ESP_ERR_INVALID_STATE;
    }
    if (!allow_source || err != ESP_OK) h->manual_otg = false;
    if (err != ESP_OK) {
        atomic_store(&h->source_allowed, false);
        (void)boost_off_locked(h); /* Best effort; return the original error. */
    }
    xSemaphoreGiveRecursive(h->mutex);
    return err;
}

esp_err_t tg29_configure_cc_detection(tg29_handle_t h, bool allow_source)
{
    return configure_otg_policy(h, allow_source, false);
}

esp_err_t tg29_configure_manual_otg(tg29_handle_t h)
{
    return configure_otg_policy(h, true, true);
}

esp_err_t tg29_get_cc_events(tg29_handle_t h, uint8_t *events)
{
    if (!events) return ESP_ERR_INVALID_ARG;
    *events = 0;
    if (!h) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    uint8_t pending = 0;
    esp_err_t err = tg29_read_reg(h, 0x4C, &pending);
    if (err == ESP_OK) {
        *events = pending & 0x60U;
        if (*events) err = tg29_write_reg(h, 0x4C, *events);
    }
    xSemaphoreGiveRecursive(h->mutex);
    return err;
}

static bool manual_input_safe(const tg29_usb_power_state_t *sample)
{
    return sample->source_allowed && sample->otg_role_ready && !sample->vbus_good &&
           !sample->boost_enabled && !sample->rbfet_enabled && (sample->adc_enable_raw & 0x04U);
}

/* Caller reserves the start and holds the mutex on entry. Every delay is
 * unlocked so OFF and other PMIC users can proceed. On a failed reacquisition
 * the caller performs checked OFF separately rather than using an unlocked bus. */
static esp_err_t settle_manual_vbus_locked(tg29_handle_t h, unsigned epoch, bool *locked)
{
    if (!source_permission_current(h) || atomic_load(&h->source_epoch) != epoch)
        return ESP_ERR_INVALID_STATE;
    uint8_t adc = 0;
    esp_err_t err = tg29_read_reg(h, TG29_REG_ADC_ENABLE, &adc);
    if (err != ESP_OK) return err;
    if (!(adc & 0x04U)) {
        h->vbus_adc_owned = true; /* A failed write may still have reached the PMIC. */
        err = tg29_write_checked(h, TG29_REG_ADC_ENABLE, adc | 0x04U, 0x04U);
        if (err != ESP_OK) return err;
    }
    const TickType_t begin = xTaskGetTickCount();
    const TickType_t settle = pdMS_TO_TICKS(TG29_MANUAL_ADC_SETTLE_MS);
    const TickType_t budget = pdMS_TO_TICKS(TG29_MANUAL_ADC_BUDGET_MS);
    const TickType_t interval = pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1;
    unsigned low_samples = 0;
    while (xTaskGetTickCount() - begin < budget) {
        if (!source_permission_current(h) || atomic_load(&h->source_epoch) != epoch)
            return ESP_ERR_INVALID_STATE;
        tg29_usb_power_state_t sample = {0};
        err = read_usb_power_locked(h, &sample, NULL);
        if (err != ESP_OK) return err;
        if (!manual_input_safe(&sample) || atomic_load(&h->source_epoch) != epoch)
            return ESP_ERR_INVALID_STATE;
        if (xTaskGetTickCount() - begin >= settle) {
            /* A zero ADC code corroborates the inactive VBUS detector and
             * safe role. It remains invalid as a measured voltage snapshot. */
            if (sample.vbus_raw > TG29_MANUAL_VBUS_MAX_MV) return ESP_ERR_INVALID_STATE;
            if (++low_samples == 2) return ESP_OK;
        }
        xSemaphoreGiveRecursive(h->mutex);
        *locked = false;
        vTaskDelay(interval);
        if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
            return ESP_ERR_TIMEOUT;
        *locked = true;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t tg29_set_otg_enabled(tg29_handle_t h, bool enabled)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (enabled && atomic_load(&h->input_token)) return ESP_ERR_INVALID_STATE;
    if (!enabled && atomic_load(&h->manual_start_active)) tg29_revoke_otg_source_permission(h);
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (enabled && atomic_load(&h->input_token)) {
        xSemaphoreGiveRecursive(h->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    bool locked = true;
    bool starting_manual = false;
    esp_err_t err = ESP_OK;
    if (!enabled) {
        if (h->manual_otg && source_permission_current(h)) tg29_revoke_otg_source_permission(h);
        err = boost_off_locked(h);
        goto done;
    }
    if (atomic_load(&h->manual_start_active)) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    const unsigned epoch = atomic_load(&h->source_epoch);
    const bool manual = h->manual_otg;
    if (!source_permission_current(h)) {
        err = ESP_ERR_INVALID_STATE;
        goto refuse;
    }
    tg29_usb_power_state_t before = {0};
    err = read_usb_power_locked(h, &before, NULL);
    if (err != ESP_OK) goto refuse;
    /* Automatic output requires stable local SOURCE/Rd. Manual output instead
     * requires verified detector/port OFF and ignores historical E7 states.
     * VBUSGOOD before our boost is enabled is conservatively treated as an
     * external supply. Once boosting, VBUSGOOD may be our own output. */
    const bool output_on = before.boost_enabled && before.rbfet_enabled;
    if (!before.otg_role_ready || (!output_on && before.vbus_good) ||
        (manual && (before.boost_enabled || before.rbfet_enabled) &&
         (!output_on || !h->manual_output_started))) {
        err = ESP_ERR_INVALID_STATE;
        goto refuse;
    }
    uint8_t boost = 0;
    err = tg29_read_reg(h, TG29_REG_BOOST_CONFIG, &boost);
    if (err != ESP_OK) goto refuse;
    const uint8_t wanted = (boost & 0x0CU) | TG29_BOOST_5126MV_500MA;
    if (boost != wanted) {
        if (manual && output_on) { err = ESP_ERR_INVALID_STATE; goto refuse; }
        err = boost_off_locked(h);
        if (err != ESP_OK) goto refuse;
        err = tg29_write_checked(h, TG29_REG_BOOST_CONFIG, wanted, 0xFF);
        if (err != ESP_OK) goto refuse;
    }
    if (manual && !output_on) {
        atomic_store(&h->manual_start_active, true);
        starting_manual = true;
        err = settle_manual_vbus_locked(h, epoch, &locked);
        if (err != ESP_OK) goto refuse;
    }
    err = prepare_fixed_ts_locked(h);
    if (err != ESP_OK) goto refuse;
    if (starting_manual) {
        tg29_usb_power_state_t final = {0};
        err = read_usb_power_locked(h, &final, NULL);
        if (err != ESP_OK) goto refuse;
        if (!manual_input_safe(&final) || final.vbus_raw > TG29_MANUAL_VBUS_MAX_MV) {
            err = ESP_ERR_INVALID_STATE;
            goto refuse;
        }
    }
    /* Re-read role immediately before the control write, not just the earlier
     * application poll. E7 cannot be frozen by a software mutex. */
    uint8_t cc = 0, control = 0, mode = 0, power = 0, detection = 0, gate = 0;
    err = tg29_read_reg(h, TG29_REG_CC_STATUS, &cc);
    if (err == ESP_OK) err = tg29_read_reg(h, TG29_REG_CC_MODE, &mode);
    if (err == ESP_OK) err = tg29_read_reg(h, TG29_REG_MODULE_ENABLE_2, &control);
    if (err == ESP_OK) err = tg29_read_reg(h, TG29_REG_POWER_STATUS, &power);
    if (err == ESP_OK) err = tg29_read_reg(h, TG29_REG_GAUGE_CONTROL, &detection);
    if (err == ESP_OK) err = tg29_read_reg(h, TG29_REG_RBFET_CONTROL, &gate);
    if (err == ESP_OK && starting_manual) err = tg29_read_reg(h, TG29_REG_CC_STATUS, &cc);
    if (err == ESP_OK && starting_manual) err = tg29_read_reg(h, TG29_REG_POWER_STATUS, &power);
    if (err != ESP_OK) goto refuse;
    if (!otg_cc_role_ready(manual, detection, mode & 3U, cc & 15U) ||
        (starting_manual && ((control & TG29_BOOST_ENABLE_BIT) || (gate & 0x01U))) ||
        ((!(control & TG29_BOOST_ENABLE_BIT) || !(gate & 0x01U)) && (power & 0x20U)) ||
        !source_permission_current(h) || atomic_load(&h->source_epoch) != epoch) {
        err = ESP_ERR_INVALID_STATE;
        goto refuse;
    }
    /* TG29 public p24 requires the RBFET source path before BOOST can reach
     * connector VBUS. Preserve all other REG13 bits; never use hidden registers. */
    if (!(gate & 0x01U)) {
        err = tg29_write_checked(h, TG29_REG_RBFET_CONTROL, gate | 0x01U, 0x01U);
        if (err != ESP_OK) goto refuse;
    }
    tg29_usb_power_state_t ready = {0};
    err = read_usb_power_locked(h, &ready, NULL);
    if (err != ESP_OK) goto refuse;
    /* After our checked gate opening, VBUSGOOD can come from this path.
     * Initial and immediately-pre-gate external-source rejection above stays;
     * do not reinterpret post-gate input-good as an independent external source. */
    if (!ready.source_allowed || !ready.otg_role_ready || !ready.rbfet_enabled) {
        err = ESP_ERR_INVALID_STATE; goto refuse;
    }
    if (!ready.boost_enabled) {
        if (!source_permission_current(h) || atomic_load(&h->source_epoch) != epoch) {
            err = ESP_ERR_INVALID_STATE;
            goto refuse;
        }
        err = tg29_write_checked(h, TG29_REG_MODULE_ENABLE_2,
                                 ready.module_enable_raw | TG29_BOOST_ENABLE_BIT, TG29_BOOST_ENABLE_BIT);
        if (err != ESP_OK) goto refuse;
    }
    err = read_usb_power_locked(h, &ready, NULL);
    if (err != ESP_OK) goto refuse;
    if (!ready.source_allowed || !ready.otg_role_ready || !ready.rbfet_enabled || !ready.boost_enabled ||
        atomic_load(&h->source_epoch) != epoch) {
        err = ESP_ERR_INVALID_STATE; goto refuse;
    }
    if (manual) h->manual_output_started = true;
    goto done;
refuse: {
        const esp_err_t off = locked ? boost_off_locked(h) : tg29_set_otg_enabled(h, false);
        if (off != ESP_OK) err = off; /* Never report a clean OFF if it failed. */
    }
done:
    if (starting_manual) atomic_store(&h->manual_start_active, false);
    if (locked) xSemaphoreGiveRecursive(h->mutex);
    return err;
}

static esp_err_t read_otg_diagnostics_locked(tg29_handle_t h, tg29_otg_diagnostics_t *state,
                                              const unsigned *epoch)
{
    tg29_otg_diagnostics_t snapshot = {0};
    esp_err_t err = read_usb_power_locked(h, &snapshot.power, epoch);
    const uint8_t regs[] = {0x50, 0x56, 0x57, 0x08, 0x48, 0x4B, 0xCD};
    uint8_t *const fields[] = {&snapshot.ts_config_raw, &snapshot.ts_work_low_raw,
        &snapshot.ts_work_high_raw, &snapshot.fault_raw, &snapshot.irq0_raw,
        &snapshot.irq3_raw, &snapshot.adc_select_raw};
    for (size_t i = 0; err == ESP_OK && i < sizeof(regs); ++i)
        err = probe_read_locked(h, regs[i], fields[i], epoch);
    if (err == ESP_OK && (snapshot.power.adc_enable_raw & 0x02U) &&
        !(snapshot.adc_select_raw & 0x03U)) {
        uint8_t high = 0, low = 0;
        err = probe_read_locked(h, 0xCE, &high, epoch);
        if (err == ESP_OK) err = probe_read_locked(h, 0xCF, &low, epoch);
        const uint16_t raw = (uint16_t)((high & 0x3FU) << 8) | low;
        snapshot.ts_voltage_valid = raw > 0 && raw <= 0x1FFFU;
        if (snapshot.ts_voltage_valid) snapshot.ts_half_mv = raw;
    }
    /* The mutex does not freeze CC hardware. Confirm the source role/control
     * at the end as well, and discard the entire probe if they changed. */
    if (err == ESP_OK && epoch) {
        uint8_t guard = 0;
        err = probe_read_locked(h, TG29_REG_CC_MODE, &guard, epoch);
        if (err == ESP_OK) err = probe_read_locked(h, TG29_REG_CC_STATUS, &guard, epoch);
        if (err == ESP_OK) err = probe_read_locked(h, TG29_REG_MODULE_ENABLE_2, &guard, epoch);
        if (err == ESP_OK) err = probe_read_locked(h, TG29_REG_GAUGE_CONTROL, &guard, epoch);
        if (err == ESP_OK) err = probe_read_locked(h, TG29_REG_RBFET_CONTROL, &guard, epoch);
    }
    if (err == ESP_OK) *state = snapshot;
    return err;
}

esp_err_t tg29_get_otg_diagnostics(tg29_handle_t h, tg29_otg_diagnostics_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    memset(state, 0, sizeof(*state));
    if (!h) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    const esp_err_t err = read_otg_diagnostics_locked(h, state, NULL);
    xSemaphoreGiveRecursive(h->mutex);
    return err;
}

static bool otg_controls_ready_locked(tg29_handle_t h, const tg29_usb_power_state_t *sample)
{
    return sample->source_allowed && sample->otg_role_ready && sample->boost_enabled &&
           sample->rbfet_enabled && (!h->manual_otg || h->manual_output_started);
}

static esp_err_t read_otg_control_sample(tg29_handle_t h, unsigned epoch, bool allow_manual,
                                       tg29_usb_power_state_t *sample)
{
    if (!source_permission_current(h) || atomic_load(&h->source_epoch) != epoch)
        return ESP_ERR_INVALID_STATE;
    memset(sample, 0, sizeof(*sample));
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t err = read_usb_power_locked(h, sample, NULL);
    if (err == ESP_OK && (!otg_controls_ready_locked(h, sample) ||
        (!allow_manual && h->manual_otg) || !source_permission_current(h) ||
        atomic_load(&h->source_epoch) != epoch)) err = ESP_ERR_INVALID_STATE;
    xSemaphoreGiveRecursive(h->mutex);
    return err;
}

esp_err_t tg29_verify_otg_output_ex(tg29_handle_t h, uint32_t timeout_ms,
                                    tg29_otg_verify_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    if (!h || !timeout_ms || timeout_ms > 2000U) {
        result->verify_error = ESP_ERR_INVALID_ARG;
        return result->verify_error;
    }
    tg29_usb_power_state_t *state = &result->power;
    bool voltage_deadline = false;
    unsigned epoch = 0;
    esp_err_t err = ESP_ERR_TIMEOUT;
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        goto failed;
    epoch = atomic_load(&h->source_epoch);
    tg29_usb_power_state_t sample = {0};
    err = read_usb_power_locked(h, &sample, NULL);
    if (err == ESP_OK) *state = sample;
    if (err == ESP_OK && !otg_controls_ready_locked(h, &sample))
        err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && !(sample.adc_enable_raw & 0x04U)) {
        h->vbus_adc_owned = true;
        err = tg29_write_checked(h, TG29_REG_ADC_ENABLE, sample.adc_enable_raw | 0x04U, 0x04U);
        if (err != ESP_OK) memset(state, 0, sizeof(*state));
    }
    xSemaphoreGiveRecursive(h->mutex);
    if (err != ESP_OK) goto failed;
    const TickType_t begin = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms) ? pdMS_TO_TICKS(timeout_ms) : 1;
    const TickType_t settle = pdMS_TO_TICKS(1000) ? pdMS_TO_TICKS(1000) : 1;
    const TickType_t interval = pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1;
    unsigned good = 0;
    while (xTaskGetTickCount() - begin < timeout) {
        err = read_otg_control_sample(h, epoch, true, &sample);
        *state = sample;
        if (err != ESP_OK) goto failed;
        if (xTaskGetTickCount() - begin >= settle && sample.vbus_voltage_valid &&
            sample.vbus_mv >= 4750U && sample.vbus_mv <= 5500U) ++good;
        else good = 0;
        if (good >= 2) {
            if (!source_permission_current(h) || atomic_load(&h->source_epoch) != epoch) { err = ESP_ERR_INVALID_STATE; goto failed; }
            *state = sample;
            return ESP_OK;
        }
        vTaskDelay(interval); /* No PMIC mutex held; OFF can revoke and clean up. */
    }
    err = ESP_ERR_TIMEOUT;
    voltage_deadline = true;
failed: {
        result->verify_error = err;
        if (voltage_deadline && source_permission_current(h) && atomic_load(&h->source_epoch) == epoch) {
            /* Do not wait for a diagnostic mutex: cancellation/cleanup wins. */
            if (xSemaphoreTakeRecursive(h->mutex, 0) == pdTRUE) {
                if (source_permission_current(h) && atomic_load(&h->source_epoch) == epoch) {
                    result->pre_cleanup_diag_attempted = true;
                    result->diag_error = read_otg_diagnostics_locked(h, &result->pre_cleanup_diag, &epoch);
                    result->pre_cleanup_diag_valid = result->diag_error == ESP_OK;
                    if (!result->pre_cleanup_diag_valid)
                        memset(&result->pre_cleanup_diag, 0, sizeof(result->pre_cleanup_diag));
                }
                xSemaphoreGiveRecursive(h->mutex);
            } else result->diag_error = ESP_ERR_TIMEOUT;
        }
        result->cleanup_error = tg29_set_otg_enabled(h, false);
        return result->verify_error; /* Preserve original cause independently. */
    }
}

esp_err_t tg29_verify_otg_output(tg29_handle_t h, uint32_t timeout_ms,
                                 tg29_usb_power_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    tg29_otg_verify_result_t result;
    const esp_err_t err = tg29_verify_otg_output_ex(h, timeout_ms, &result);
    *state = result.power;
    /* Compatibility: the original API prioritizes a failed checked OFF. */
    return result.cleanup_error == ESP_OK ? err : result.cleanup_error;
}

esp_err_t tg29_settle_otg_control(tg29_handle_t h, uint32_t settle_ms,
                                  tg29_otg_verify_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    if (!h || settle_ms < 100U || settle_ms > 300U) {
        result->verify_error = ESP_ERR_INVALID_ARG;
        return result->verify_error;
    }
    const unsigned epoch = atomic_load(&h->source_epoch);
    const TickType_t begin = xTaskGetTickCount();
    const TickType_t duration = pdMS_TO_TICKS(settle_ms) ? pdMS_TO_TICKS(settle_ms) : 1;
    const TickType_t interval = pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1;
    for (;;) {
        result->verify_error = read_otg_control_sample(h, epoch, false, &result->power);
        if (result->verify_error != ESP_OK) {
            result->cleanup_error = tg29_set_otg_enabled(h, false);
            return result->verify_error;
        }
        if (xTaskGetTickCount() - begin >= duration) return ESP_OK;
        vTaskDelay(interval); /* Control settling only: never claim measured5V. */
    }
}

static bool vbus_input_probe_allowed(const tg29_usb_power_state_t *power)
{
    return power->snapshot_valid && !power->source_allowed &&
           !power->boost_enabled && power->vbus_good;
}

esp_err_t tg29_probe_vbus_input(tg29_handle_t h, uint32_t timeout_ms,
                                tg29_vbus_input_probe_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    if (!h || timeout_ms < 100U || timeout_ms > 2500U) {
        result->probe_error = ESP_ERR_INVALID_ARG;
        return result->probe_error;
    }
    const unsigned epoch = atomic_load(&h->source_epoch);
    esp_err_t err = ESP_ERR_TIMEOUT;
    bool reserved = false;
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        goto cleanup;
    if (atomic_load(&h->input_token) || atomic_load(&h->input_probe_active)) {
        xSemaphoreGiveRecursive(h->mutex);
        result->probe_error = ESP_ERR_INVALID_STATE;
        return result->probe_error;
    }
    atomic_store(&h->input_probe_active, true);
    reserved = true;
    err = read_usb_power_locked(h, &result->power, NULL);
    if (err == ESP_OK && !vbus_input_probe_allowed(&result->power)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        result->attempted = true;
        if (!(result->power.adc_enable_raw & 0x04U)) {
            h->vbus_adc_owned = true;
            err = tg29_write_checked(h, TG29_REG_ADC_ENABLE, result->power.adc_enable_raw | 0x04U, 0x04U);
            if (err != ESP_OK) memset(&result->power, 0, sizeof(result->power));
        }
    }
    xSemaphoreGiveRecursive(h->mutex);
    if (err != ESP_OK) goto cleanup;
    const TickType_t begin = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms) ? pdMS_TO_TICKS(timeout_ms) : 1;
    const TickType_t interval = pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1;
    static const uint32_t milestones[TG29_VBUS_INPUT_TRACE_MAX] = {100,250,500,1000,2000};
    for (;;) {
        if (atomic_load(&h->source_epoch) != epoch || source_permission_current(h)) {
            err = ESP_ERR_INVALID_STATE; break;
        }
        tg29_usb_power_state_t sample;
        const esp_err_t read_error = tg29_get_usb_power_state(h, &sample);
        if (read_error != ESP_OK) { memset(&result->power, 0, sizeof(result->power)); err = read_error; break; }
        result->power = sample;
        if (!vbus_input_probe_allowed(&sample) || atomic_load(&h->source_epoch) != epoch) {
            err = ESP_ERR_INVALID_STATE; break;
        }
        if (!(sample.adc_enable_raw & 0x04U)) { err = ESP_ERR_INVALID_STATE; break; }
        const TickType_t elapsed = xTaskGetTickCount() - begin;
        const uint32_t elapsed_ms = (uint32_t)(((uint64_t)elapsed * 1000U) / configTICK_RATE_HZ);
        if (elapsed_ms >= 100U && sample.vbus_voltage_valid && !result->first_valid_elapsed_ms)
            result->first_valid_elapsed_ms = elapsed_ms;
        while (result->trace_count < TG29_VBUS_INPUT_TRACE_MAX &&
               milestones[result->trace_count] <= timeout_ms &&
               elapsed_ms >= milestones[result->trace_count]) {
            result->trace[result->trace_count++] = (tg29_vbus_input_trace_t){
                .elapsed_ms = elapsed_ms, .adc_enable_raw = sample.adc_enable_raw,
                .vbus_adc_high_raw = sample.vbus_adc_high_raw,
                .vbus_adc_low_raw = sample.vbus_adc_low_raw,
                .vbus_raw = sample.vbus_raw, .vbus_mv = sample.vbus_mv,
                .vbus_voltage_valid = sample.vbus_voltage_valid,
            };
        }
        if (elapsed >= timeout) { err = ESP_OK; break; }
        vTaskDelay(interval);
    }
cleanup:
    result->probe_error = err;
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        result->cleanup_error = restore_owned_vbus_adc_locked(h);
        xSemaphoreGiveRecursive(h->mutex);
    } else result->cleanup_error = ESP_ERR_TIMEOUT;
    if (reserved) atomic_store(&h->input_probe_active, false);
    return result->probe_error;
}

static esp_err_t read_charge_current_locked(tg29_handle_t h,
                                             tg29_charge_current_state_t *state)
{
    uint8_t fast = 0, pre = 0;
    esp_err_t err = tg29_read_reg(h, TG29_REG_FAST_CHARGE, &fast);
    if (err == ESP_OK) err = tg29_read_reg(h, TG29_REG_PRECHARGE, &pre);
    if (err != ESP_OK) return err;
    const uint8_t f = fast & 0x3FU, p = pre & 0x0FU;
    *state = (tg29_charge_current_state_t){
        .fast_charge_code = f, .precharge_code = p,
        .fast_charge_ma = (uint16_t)(f * 64U), .precharge_ma = (uint16_t)(p * 64U),
        .fast_charge_valid = f < 48U, .precharge_valid = true,
    };
    return state->fast_charge_valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t tg29_get_charge_current(tg29_handle_t h, tg29_charge_current_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    memset(state, 0, sizeof(*state));
    if (!h) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    tg29_charge_current_state_t snapshot = {0};
    const esp_err_t err = read_charge_current_locked(h, &snapshot);
    if (err == ESP_OK || err == ESP_ERR_INVALID_RESPONSE) *state = snapshot;
    xSemaphoreGiveRecursive(h->mutex);
    return err;
}

esp_err_t tg29_limit_charge_current(tg29_handle_t h, uint16_t maximum_ma,
                                    tg29_charge_current_state_t *state, bool *changed)
{
    if (state) memset(state, 0, sizeof(*state));
    if (changed) *changed = false;
    if (!h || !state || !changed || maximum_ma < 64U || maximum_ma > 3008U)
        return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    uint8_t values[2] = {0};
    const uint8_t regs[] = {TG29_REG_FAST_CHARGE, TG29_REG_PRECHARGE};
    const uint8_t masks[] = {0x3FU, 0x0FU};
    const uint8_t ceiling = maximum_ma / 64U;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < 2; ++i) {
        err = tg29_read_reg(h, regs[i], &values[i]);
        if (err != ESP_OK) goto done;
    }
    for (size_t i = 0; i < 2; ++i) {
        if ((values[i] & masks[i]) <= ceiling) continue;
        err = tg29_write_checked(h, regs[i],
                                 (values[i] & (uint8_t)~masks[i]) | ceiling, masks[i]);
        if (err != ESP_OK) goto done;
        *changed = true;
    }
    err = read_charge_current_locked(h, state);
    if (err != ESP_OK) memset(state, 0, sizeof(*state));
done:
    xSemaphoreGiveRecursive(h->mutex);
    return err;
}

esp_err_t tg29_disable_charging(tg29_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTakeRecursive(h->mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    uint8_t control = 0;
    esp_err_t err = tg29_read_reg(h, TG29_REG_MODULE_ENABLE_2, &control);
    if (err == ESP_OK && (control & 0x02U))
        err = tg29_write_checked(h, TG29_REG_MODULE_ENABLE_2,
                                 control & (uint8_t)~0x02U, 0x02U);
    xSemaphoreGiveRecursive(h->mutex);
    return err;
}

const char *tg29_rail_name(tg29_rail_t rail)
{
    static const char *const names[] = {
        "DCDC1", "DCDC2", "DCDC3", "ALDO1", "ALDO2", "ALDO3", "ALDO4",
        "BLDO1", "BLDO2", "BLDO3", "BLDO4", "CLDO1", "CLDO2", "CLDO3",
        "CLDO4", "CPUSLDO",
    };
    return (rail >= 0 && rail < (int)(sizeof(names) / sizeof(names[0]))) ? names[rail] : "UNKNOWN";
}
