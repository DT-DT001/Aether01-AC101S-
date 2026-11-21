#include "ac101s_codec.h"

#include <stdint.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Register layout and I2S format from the AC101S codec in the Allwinner
 * R128 SDK, revision 2941d13fa48e43e96c4351a85bc6aa2a5a556ca6. */
enum {
    AC101S_I2C_ADDRESS = 0x33, /* DEVID low; schematic shows write byte 0x66. */
    AC101S_IO_MS = 10,
    REG_RESET = 0x00, REG_POWER1 = 0x01, REG_POWER2 = 0x02,
    REG_FUNCTION = 0x03, REG_ADC_DIV = 0x04, REG_DAC_DIV = 0x05, REG_CLOCK = 0x06,
    REG_I2S = 0x07, REG_BCLK = 0x08, REG_LRCK_HIGH = 0x09, REG_LRCK_LOW = 0x0a,
    REG_FMT1 = 0x0b, REG_FMT2 = 0x0c, REG_SLOTS = 0x0e, REG_TX_CHANNELS = 0x0f,
    REG_RX_MIX = 0x18, REG_ADC_DIG = 0x19, REG_ADC_VOLUME = 0x1a, REG_DAC_DIG = 0x1b,
    REG_ADC_ANA = 0x20, REG_DAC_ANA = 0x25, REG_LINEOUT = 0x26,
    MIC_BIAS = 1 << 2, ADC_RECORD = 1 << 4, DAC_PLAY_AUTO = 1 << 5, DAC_ANALOG_OUT = 1,
    SYS_CLOCK = 1 << 5, I2S_CLOCK = 1 << 4, DAC_CLOCK = 1 << 2, ADC_CLOCK = 1,
    I2S_SDO = 1 << 4, I2S_TX = 1 << 2, I2S_RX = 1 << 1, I2S_GLOBAL = 1,
    ADC_PGA_MASK = 0xf8, ADC_ANALOG_ENABLE = 1, ADC_DIGITAL_ENABLE = 1,
    DAC_DIGITAL_ENABLE = 1, DAC_REFERENCE = 3 << 5, LINEOUT_MUTE = 1 << 2,
    LINEOUT_ENABLE = 1 << 5, LINEOUT_SINGLE_ENDED = 1 << 4, LINEOUT_GAIN_MASK = 0x0f,
};

static const char *const TAG = "ac101s";

typedef struct {
    uint8_t reg, mask, value;
} setting_t;

static esp_err_t write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(dev, data, sizeof(data), AC101S_IO_MS);
}

static esp_err_t apply(i2c_master_dev_handle_t dev, setting_t setting)
{
    uint8_t value = 0;
    esp_err_t err = i2c_master_transmit_receive(dev, &setting.reg, 1, &value, 1, AC101S_IO_MS);
    if (err != ESP_OK) return err;
    if ((value & setting.mask) == setting.value) return ESP_OK;
    err = write_reg(dev, setting.reg, (value & ~setting.mask) | setting.value);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit_receive(dev, &setting.reg, 1, &value, 1, AC101S_IO_MS);
    if (err != ESP_OK) return err;
    return (value & setting.mask) == setting.value ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t apply_settings(i2c_master_dev_handle_t dev, const setting_t *settings,
                                size_t count, bool finish_on_error)
{
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < count; ++i) {
        const esp_err_t err = apply(dev, settings[i]);
        if (err == ESP_OK) continue;
        if (result == ESP_OK) result = err;
        ESP_LOGW(TAG, "I2C register 0x%02x error: %s", settings[i].reg, esp_err_to_name(err));
        if (!finish_on_error) break;
    }
    return result;
}

esp_err_t ac101s_codec_new(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t *out)
{
    *out = NULL;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AC101S_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &dev), TAG, "add device");
    const setting_t init[] = {
        {REG_POWER1, 0xff, 0x73}, /* Internal analog 1.8 V, digital 1.2 V, bias 2.39 V. */
        {REG_POWER2, 0x1f, 0x1b}, /* LDOs and references, microphone bias off. */
        {REG_DAC_ANA, LINEOUT_MUTE, LINEOUT_MUTE},
        {REG_FUNCTION, ADC_RECORD | DAC_PLAY_AUTO | DAC_ANALOG_OUT, 0},
        {REG_ADC_DIV, 0x0f, 1}, {REG_DAC_DIV, 0x0f, 1}, /* MCLK / 2 = 128 fs. */
        {REG_I2S, 0xd7, 0}, /* ESP32-S31 drives BCLK and LRCK. */
        {REG_BCLK, 0x10, 0}, {REG_LRCK_HIGH, 0x13, 0},
        {REG_LRCK_LOW, 0xff, 15}, /* 16 BCLKs per LRCK half-frame. */
        {REG_FMT1, 0x34, 0x14}, /* Philips I2S, one-bit delay. */
        {REG_FMT2, 0x77, 0x33}, /* 16-bit slot and 16-bit sample. */
        {REG_SLOTS, 0x0f, 0x05}, {REG_TX_CHANNELS, 0x0f, 0x03},
        {REG_RX_MIX, 0x0f, 0x0f}, /* Attenuated sum of the two playback channels. */
        {REG_ADC_VOLUME, 0xff, 0x81}, /* ADC digital unity gain. */
        {REG_LINEOUT, LINEOUT_SINGLE_ENDED, 0}, /* Differential PA input. */
    };
    esp_err_t err = write_reg(dev, REG_RESET, 0x34);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20) + 1);
        err = apply_settings(dev, init, sizeof(init) / sizeof(init[0]), false);
    }
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(dev);
        return err;
    }
    *out = dev;
    return ESP_OK;
}

esp_err_t ac101s_codec_start(i2c_master_dev_handle_t dev, bool capture_only, float gain_db)
{
    const uint8_t pga = (uint8_t)(gain_db > 31.0f ? 31.0f : gain_db);
    const setting_t settings[] = {
        {REG_DAC_ANA, LINEOUT_MUTE, LINEOUT_MUTE},
        {REG_POWER2, MIC_BIAS, MIC_BIAS},
        {REG_CLOCK, SYS_CLOCK | I2S_CLOCK | ADC_CLOCK | DAC_CLOCK,
                    SYS_CLOCK | I2S_CLOCK | ADC_CLOCK | (capture_only ? 0 : DAC_CLOCK)},
        {REG_ADC_ANA, ADC_PGA_MASK | ADC_ANALOG_ENABLE, (pga << 3) | ADC_ANALOG_ENABLE},
        {REG_ADC_DIG, 0x81, ADC_DIGITAL_ENABLE}, /* Analog microphone, not PDM. */
        {REG_DAC_ANA, DAC_REFERENCE, capture_only ? 0 : DAC_REFERENCE},
        {REG_DAC_DIG, DAC_DIGITAL_ENABLE, capture_only ? 0 : DAC_DIGITAL_ENABLE},
        {REG_FUNCTION, ADC_RECORD | DAC_ANALOG_OUT, ADC_RECORD | (capture_only ? 0 : DAC_ANALOG_OUT)},
        {REG_LINEOUT, LINEOUT_ENABLE, capture_only ? 0 : LINEOUT_ENABLE},
        {REG_I2S, I2S_SDO | I2S_TX | I2S_RX | I2S_GLOBAL,
                  I2S_SDO | I2S_TX | I2S_GLOBAL | (capture_only ? 0 : I2S_RX)},
    };
    const esp_err_t err = apply_settings(dev, settings, sizeof(settings) / sizeof(settings[0]), false);
    if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(20) + 1);
    return err;
}

esp_err_t ac101s_codec_set_speaker(i2c_master_dev_handle_t dev, unsigned level)
{
    enum { MAX_LEVEL = 70, QUIETEST_CODE = 15, MAX_SPEAKER_CODE = 5 };
    if (level > MAX_LEVEL) return ESP_ERR_INVALID_ARG;
    /* Higher line-output codes attenuate the speaker more on this board.
     * Use the former 50% setting (code 5) as the speaker ceiling. */
    const unsigned steps = level * (QUIETEST_CODE - MAX_SPEAKER_CODE) / MAX_LEVEL;
    const uint8_t gain = (uint8_t)(QUIETEST_CODE - steps);
    const setting_t settings[] = {
        {REG_LINEOUT, LINEOUT_GAIN_MASK, gain},
        {REG_DAC_ANA, LINEOUT_MUTE, level ? 0 : LINEOUT_MUTE},
    };
    return apply_settings(dev, settings, sizeof(settings) / sizeof(settings[0]), false);
}

esp_err_t ac101s_codec_stop(i2c_master_dev_handle_t dev)
{
    const setting_t settings[] = {
        {REG_DAC_ANA, LINEOUT_MUTE, LINEOUT_MUTE},
        {REG_LINEOUT, LINEOUT_ENABLE, 0},
        {REG_I2S, I2S_SDO | I2S_TX | I2S_RX | I2S_GLOBAL, 0},
        {REG_FUNCTION, ADC_RECORD | DAC_ANALOG_OUT, 0},
        {REG_ADC_DIG, ADC_DIGITAL_ENABLE, 0}, {REG_ADC_ANA, ADC_ANALOG_ENABLE, 0},
        {REG_DAC_DIG, DAC_DIGITAL_ENABLE, 0}, {REG_DAC_ANA, DAC_REFERENCE, 0},
        {REG_POWER2, MIC_BIAS, 0},
        {REG_CLOCK, SYS_CLOCK | I2S_CLOCK | ADC_CLOCK | DAC_CLOCK, 0},
    };
    return apply_settings(dev, settings, sizeof(settings) / sizeof(settings[0]), true);
}
