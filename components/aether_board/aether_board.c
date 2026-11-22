#include "aether_board.h"

#include <string.h>
#include "aether_board_pins.h"
#include "co5300_display.h"
#include "cst9220.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

static const char *TAG = "aether_board";

_Static_assert(SOC_HP_I2C_NUM >= 2, "Aether01 requires two HP I2C controllers");
_Static_assert(SOC_LP_I2C_NUM >= 1, "Aether01 system bus requires LP I2C");

static esp_err_t configure_safe_outputs(void)
{
    const gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << AETHER_GPIO_PA_ENABLE) |
                        (1ULL << AETHER_GPIO_LCD_AVDD_EN) |
                        (1ULL << AETHER_GPIO_LCD_RST) |
                        (1ULL << AETHER_GPIO_TOUCH_RST) |
                        (1ULL << AETHER_GPIO_USB_SELECT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "configure safe outputs");
    gpio_set_level(AETHER_GPIO_PA_ENABLE, 0);
    gpio_set_level(AETHER_GPIO_LCD_AVDD_EN, 0);
    gpio_set_level(AETHER_GPIO_LCD_RST, 0);
    gpio_set_level(AETHER_GPIO_TOUCH_RST, 0);
    gpio_set_level(AETHER_GPIO_USB_SELECT, 0);
    return ESP_OK;
}

static void hold_failed_output_low(gpio_num_t pin, const char *name)
{
    /* A failed peripheral constructor may have reset its GPIO to an input.
     * Set the output latch before restoring output mode to avoid a high pulse. */
    esp_err_t err = gpio_set_level(pin, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failure cleanup %s low: %s", name, esp_err_to_name(err));
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failure cleanup %s output: %s", name, esp_err_to_name(err));
    }
}

static void disable_failed_rail(aether_board_t *board, tg29_rail_t rail)
{
    const esp_err_t err = tg29_set_rail_enabled(board->pmic, rail, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failure cleanup %s off: %s", tg29_rail_name(rail),
                 esp_err_to_name(err));
    }
}

static esp_err_t new_i2c_bus(i2c_port_num_t port, const char *name,
                             gpio_num_t sda, gpio_num_t scl,
                             i2c_master_bus_handle_t *out_bus)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .glitch_ignore_cnt = 7,
        /* All three buses have schematic pull-ups in their own power domains. */
        .flags.enable_internal_pullup = false,
    };
    if (port == LP_I2C_NUM_0) {
        cfg.lp_source_clk = LP_I2C_SCLK_DEFAULT;
    } else {
        cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    }
    esp_err_t err = i2c_new_master_bus(&cfg, out_bus);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s I2C: port=%d type=%s SDA=%d SCL=%d", name, port,
                 port == LP_I2C_NUM_0 ? "LP" : "HP", sda, scl);
    }
    return err;
}

static void probe_sensors(aether_board_t *board)
{
    esp_err_t err = qmi8658a_new(board->system_lp_i2c, &board->imu);
    board->imu_ready = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658A probe failed: %s", esp_err_to_name(err));
    }

    err = dps368_new(board->system_lp_i2c, &board->barometer);
    board->barometer_ready = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DPS368 probe failed: %s", esp_err_to_name(err));
    }

    err = pcf85063_new(board->system_lp_i2c, &board->rtc);
    board->rtc_ready = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 probe failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t init_audio(aether_board_t *board)
{
    ESP_RETURN_ON_ERROR(new_i2c_bus(AETHER_I2C_PORT_AUDIO, "audio",
                                    AETHER_GPIO_AUDIO_I2C_SDA,
                                    AETHER_GPIO_AUDIO_I2C_SCL,
                                    &board->audio_i2c), TAG, "create audio I2C bus");
    const ac101s_audio_config_t cfg = {
        .i2c_bus = board->audio_i2c,
        .mclk = AETHER_GPIO_AC101S_MCLK,
        .bclk = AETHER_GPIO_AC101S_BCLK,
        .ws = AETHER_GPIO_AC101S_LRCK,
        /* I2S directions below are from the ESP32-S31 point of view. */
        .dout = AETHER_GPIO_AC101S_SDIN,
        .din = AETHER_GPIO_AC101S_SDOUT,
        .pa_enable = AETHER_GPIO_PA_ENABLE,
        .sample_rate_hz = 48000,
    };
    return ac101s_audio_new(&cfg, &board->audio);
}

static esp_err_t init_display(aether_board_t *board)
{
    esp_err_t err = gpio_set_level(AETHER_GPIO_LCD_AVDD_EN, 1);
    if (err != ESP_OK) goto failed;
    vTaskDelay(pdMS_TO_TICKS(20));
    const co5300_display_config_t cfg = {
        .host = SPI2_HOST,
        .clk = AETHER_GPIO_LCD_QSPI_CLK,
        .data0 = AETHER_GPIO_LCD_QSPI_D0,
        .data1 = AETHER_GPIO_LCD_QSPI_D1,
        .data2 = AETHER_GPIO_LCD_QSPI_D2,
        .data3 = AETHER_GPIO_LCD_QSPI_D3,
        .cs = AETHER_GPIO_LCD_QSPI_CS,
        .reset = AETHER_GPIO_LCD_RST,
        .te = AETHER_GPIO_LCD_TE,
        .clock_hz = CONFIG_AETHER_LCD_PCLK_HZ,
        .width = CONFIG_AETHER_LCD_H_RES,
        .height = CONFIG_AETHER_LCD_V_RES,
        .max_transfer_bytes = CONFIG_AETHER_LCD_H_RES * CONFIG_AETHER_LCD_BUFFER_LINES * 2,
    };
    err = co5300_display_new(&cfg, &board->display_io, &board->display);
    if (err == ESP_OK) return ESP_OK;

failed:
    /* Preserve the initialization error even if a cleanup operation fails. */
    hold_failed_output_low(AETHER_GPIO_LCD_AVDD_EN, "AVDD_EN");
    hold_failed_output_low(AETHER_GPIO_LCD_RST, "LCD_RST");
    disable_failed_rail(board, TG29_RAIL_CLDO3);
    return err;
}

static esp_err_t init_touch(aether_board_t *board)
{
    ESP_RETURN_ON_ERROR(new_i2c_bus(AETHER_I2C_PORT_TOUCH, "touch",
                                    AETHER_GPIO_TOUCH_SDA,
                                    AETHER_GPIO_TOUCH_SCL,
                                    &board->touch_i2c), TAG, "create touch I2C bus");
    const esp_lcd_touch_config_t cfg = {
        .x_max = CONFIG_AETHER_LCD_H_RES,
        .y_max = CONFIG_AETHER_LCD_V_RES,
        .rst_gpio_num = AETHER_GPIO_TOUCH_RST,
        .int_gpio_num = AETHER_GPIO_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
    };
    return cst9220_new(board->touch_i2c, &cfg, &board->touch);
}

static void compare_system_bus_with_hp(aether_board_t *board)
{
    /* No devices or tasks exist yet. Temporarily compare controllers on the
     * same wires; never use the audio controller as a persistent system bus. */
    esp_err_t err = i2c_del_master_bus(board->system_lp_i2c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LP diagnostic bus release failed: %s", esp_err_to_name(err));
        return;
    }
    board->system_lp_i2c = NULL;
    const gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << AETHER_GPIO_SYS_I2C_SDA) | (1ULL << AETHER_GPIO_SYS_I2C_SCL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&inputs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "system bus diagnostic input setup: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "system wires with LP released: SDA=%d SCL=%d",
             gpio_get_level(AETHER_GPIO_SYS_I2C_SDA), gpio_get_level(AETHER_GPIO_SYS_I2C_SCL));
    i2c_master_bus_handle_t diagnostic_bus = NULL;
    err = new_i2c_bus(I2C_NUM_0, "system diagnostic only", AETHER_GPIO_SYS_I2C_SDA,
                       AETHER_GPIO_SYS_I2C_SCL, &diagnostic_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HP comparison bus unavailable: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "system HP I2C idle: SDA=%d SCL=%d",
             gpio_get_level(AETHER_GPIO_SYS_I2C_SDA), gpio_get_level(AETHER_GPIO_SYS_I2C_SCL));
    const esp_err_t pmic_err = i2c_master_probe(diagnostic_bus, TG29_I2C_ADDRESS_7BIT, 100);
    const esp_err_t rtc_err = i2c_master_probe(diagnostic_bus, 0x51, 100);
    ESP_LOGI(TAG, "HP comparison (no register writes): TG29=%s RTC=%s",
             esp_err_to_name(pmic_err), esp_err_to_name(rtc_err));
    err = i2c_del_master_bus(diagnostic_bus);
    if (err != ESP_OK) ESP_LOGW(TAG, "HP diagnostic bus release: %s", esp_err_to_name(err));
}

static esp_err_t probe_system_power_bus(aether_board_t *board)
{
    ESP_LOGI(TAG, "system LP I2C idle: SDA(GPIO%d)=%d SCL(GPIO%d)=%d",
             AETHER_GPIO_SYS_I2C_SDA, rtc_gpio_get_level(AETHER_GPIO_SYS_I2C_SDA),
             AETHER_GPIO_SYS_I2C_SCL, rtc_gpio_get_level(AETHER_GPIO_SYS_I2C_SCL));
    /* Address-only probes, at the IDF probe's fixed 100 kHz. No register data
     * is written. RTC has an always-on supply; sensor LDOs may still be off. */
    const esp_err_t pmic_err = i2c_master_probe(board->system_lp_i2c, TG29_I2C_ADDRESS_7BIT, 100);
    ESP_LOGI(TAG, "TG29 address probe 0x%02X: %s", TG29_I2C_ADDRESS_7BIT,
             esp_err_to_name(pmic_err));
    if (pmic_err != ESP_OK) {
        const esp_err_t rtc_err = i2c_master_probe(board->system_lp_i2c, 0x51, 100);
        ESP_LOGI(TAG, "RTC address probe 0x51: %s", esp_err_to_name(rtc_err));
        compare_system_bus_with_hp(board);
        ESP_LOGE(TAG, "PMIC probe failed; display power remains disabled. Check system I2C pins/pull-ups/supply.");
    }
    return pmic_err;
}

esp_err_t aether_board_init_display(aether_board_t *board)
{
    ESP_RETURN_ON_FALSE(board, ESP_ERR_INVALID_ARG, TAG, "invalid board pointer");
    memset(board, 0, sizeof(*board));
    ESP_RETURN_ON_ERROR(configure_safe_outputs(), TAG, "safe GPIO state");
    ESP_RETURN_ON_ERROR(new_i2c_bus(AETHER_I2C_PORT_SYSTEM, "system",
                                    AETHER_GPIO_SYS_I2C_SDA,
                                    AETHER_GPIO_SYS_I2C_SCL,
                                    &board->system_lp_i2c), TAG, "create system LP I2C bus");
    ESP_RETURN_ON_ERROR(probe_system_power_bus(board), TAG, "probe system power bus");
    ESP_RETURN_ON_ERROR(tg29_new(board->system_lp_i2c, &board->pmic), TAG, "probe TG29");
    /* Schematic R311 is a fixed 10k pull-down, not a battery NTC input.
     * Allow a temporary 50uA TS bias during OTG; idle bias remains off. */
    ESP_RETURN_ON_ERROR(tg29_set_otg_fixed_ts_10k_policy(board->pmic, true), TAG,
                        "declare board OTG TS resistor");
    /* A warm MCU reset must first revoke a previous OTG source policy. */
    const esp_err_t cc_error = tg29_configure_cc_detection(board->pmic, false);
    tg29_charge_current_state_t charge;
    bool charge_changed = false;
    const esp_err_t charge_error = tg29_limit_charge_current(board->pmic, 800, &charge, &charge_changed);
    if (charge_error != ESP_OK) {
        const esp_err_t disabled = tg29_disable_charging(board->pmic);
        ESP_LOGE(TAG, "charge limit unverified: %s; disable charging: %s",
                 esp_err_to_name(charge_error), esp_err_to_name(disabled));
        return charge_error;
    }
    ESP_LOGI(TAG, "charge programmed limits: fast=%u mA precharge=%u mA changed=%u (ceiling800 mA)",
             charge.fast_charge_ma, charge.precharge_ma, charge_changed);
    ESP_RETURN_ON_ERROR(cc_error, TAG, "default USB sink and boost OFF policy");

    const tg29_power_profile_t profile = {
        .aldo2_mv = CONFIG_AETHER_POWER_ALDO2_MV,
        .aldo4_mv = CONFIG_AETHER_POWER_ALDO4_MV,
        .bldo1_mv = CONFIG_AETHER_POWER_BLDO1_MV,
        .cldo1_mv = CONFIG_AETHER_POWER_CLDO1_MV,
        .cldo3_mv = CONFIG_AETHER_POWER_CLDO3_MV,
        /* Sensor I/O remains on the PMIC's system I2C bus even when sampling
         * is disabled. Keep ALDO2 powered so those pins cannot clamp the bus. */
        .enable_sensor_rail = true,
        .enable_codec_rail = CONFIG_AETHER_ENABLE_AUDIO,
        .enable_sd_rail = false,
        .enable_touch_rail = CONFIG_AETHER_ENABLE_TOUCH,
        .enable_display_rail = CONFIG_AETHER_ENABLE_DISPLAY,
    };
    ESP_RETURN_ON_ERROR(tg29_apply_power_profile(board->pmic, &profile), TAG,
                        "apply TG29 power policy");
    vTaskDelay(pdMS_TO_TICKS(10));

    if (CONFIG_AETHER_ENABLE_DISPLAY) {
        ESP_RETURN_ON_ERROR(init_display(board), TAG, "initialize CO5300AF");
        board->display_ready = true;
        board->display_brightness = 100;
    }
    return ESP_OK;
}

esp_err_t aether_board_init_peripherals(aether_board_t *board)
{
    ESP_RETURN_ON_FALSE(board && board->pmic, ESP_ERR_INVALID_ARG, TAG, "board power unavailable");
    /* Before USB host or power tasks exist, test the ADC against an external USB input.
     * This probe never enables the boost and restores its temporary channel. */
    tg29_vbus_input_probe_result_t input_probe = {0};
    const esp_err_t input_error = tg29_probe_vbus_input(board->pmic, 2000, &input_probe);
    if (input_probe.attempted) {
        ESP_LOGI(TAG, "USB input ADC: result=%s cleanup=%s sample=%u R19=%02X R13=%02X C0=%02X C6=%02X C7=%02X raw=%u valid=%u VBUS=%u mV",
                 esp_err_to_name(input_error), esp_err_to_name(input_probe.cleanup_error),
                 input_probe.power.snapshot_valid, input_probe.power.module_enable_raw,
                 input_probe.power.rbfet_control_raw, input_probe.power.adc_enable_raw, input_probe.power.vbus_adc_high_raw,
                 input_probe.power.vbus_adc_low_raw, input_probe.power.vbus_raw,
                 input_probe.power.vbus_voltage_valid, input_probe.power.vbus_mv);
        ESP_LOGI(TAG, "USB input ADC timeline: samples=%u first_valid=%lu ms",
                 input_probe.trace_count, (unsigned long)input_probe.first_valid_elapsed_ms);
        for (unsigned i = 0; i < input_probe.trace_count; ++i) {
            const tg29_vbus_input_trace_t *point = &input_probe.trace[i];
            ESP_LOGI(TAG, "USB input ADC t=%lu ms C0=%02X C6=%02X C7=%02X raw=%u valid=%u decoded_mv=%u",
                     (unsigned long)point->elapsed_ms, point->adc_enable_raw,
                     point->vbus_adc_high_raw, point->vbus_adc_low_raw,
                     point->vbus_raw, point->vbus_voltage_valid, point->vbus_mv);
        }
    } else if (input_error != ESP_OK && input_error != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB input ADC probe unavailable: %s", esp_err_to_name(input_error));
    }
    board->otg_vbus_adc_trusted = input_error == ESP_OK && input_probe.cleanup_error == ESP_OK &&
        input_probe.attempted && input_probe.power.snapshot_valid &&
        input_probe.power.vbus_voltage_valid && input_probe.power.vbus_mv >= 3900 &&
        input_probe.power.vbus_mv <= 5500;
    ESP_LOGI(TAG, "OTG VBUS ADC trusted=%u; Host startup uses %s verification",
             board->otg_vbus_adc_trusted,
             board->otg_vbus_adc_trusted ? "measured voltage" : "CC and BOOST control");
    if (input_probe.cleanup_error != ESP_OK) {
        const esp_err_t cleanup = tg29_configure_cc_detection(board->pmic, false);
        ESP_LOGW(TAG, "USB input ADC restore=%s; retry checked OFF=%s",
                 esp_err_to_name(input_probe.cleanup_error), esp_err_to_name(cleanup));
    }

    const gpio_config_t board_inputs = {
        .pin_bit_mask = (1ULL << AETHER_GPIO_DPS368_INT) |
                        (1ULL << AETHER_GPIO_QMI8658_INT1) |
                        (1ULL << AETHER_GPIO_RTC_INT) |
                        (1ULL << AETHER_GPIO_TG29_INT) |
                        (1ULL << AETHER_GPIO_TF_DET) |
                        (1ULL << AETHER_GPIO_BOOT_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&board_inputs), TAG, "configure board inputs");

    /* IRQ isolation diodes have their anodes on the MCU side. The pull-ups
     * on the RTC/PMIC side cannot drive these GPIOs high through a reverse-
     * biased diode, so each MCU input needs its own system-domain pull-up. */
    const gpio_config_t isolated_irq_inputs = {
        .pin_bit_mask = (1ULL << AETHER_GPIO_RTC_INT) |
                        (1ULL << AETHER_GPIO_TG29_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&isolated_irq_inputs), TAG, "configure isolated IRQ pull-ups");

    if (CONFIG_AETHER_ENABLE_SENSORS) {
        probe_sensors(board);
    }
    if (CONFIG_AETHER_ENABLE_AUDIO) {
        esp_err_t err = init_audio(board);
        board->audio_ready = err == ESP_OK;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AC101S path initialization failed: %s", esp_err_to_name(err));
            hold_failed_output_low(AETHER_GPIO_PA_ENABLE, "PA_ENABLE");
            disable_failed_rail(board, TG29_RAIL_ALDO4);
        }
    }
    if (CONFIG_AETHER_ENABLE_TOUCH) {
        esp_err_t err = init_touch(board);
        board->touch_ready = err == ESP_OK;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "CST9220 initialization failed: %s", esp_err_to_name(err));
            hold_failed_output_low(AETHER_GPIO_TOUCH_RST, "TP_RST");
            /* No touch task exists yet. The failed driver constructor removes
             * its device; deletion refuses a bus with any remaining device. */
            if (board->touch_i2c) {
                const esp_err_t cleanup_err = i2c_del_master_bus(board->touch_i2c);
                if (cleanup_err == ESP_OK) {
                    board->touch_i2c = NULL;
                } else {
                    ESP_LOGW(TAG, "failure cleanup touch I2C bus: %s",
                             esp_err_to_name(cleanup_err));
                }
            }
            disable_failed_rail(board, TG29_RAIL_CLDO1);
        }
    }

    ESP_LOGI(TAG, "board initialized: display=%d touch=%d audio=%d imu=%d baro=%d rtc=%d",
             board->display_ready, board->touch_ready, board->audio_ready,
             board->imu_ready, board->barometer_ready,
             board->rtc_ready);
    return ESP_OK;
}

esp_err_t aether_board_set_sd_power(aether_board_t *board, bool enabled)
{
    ESP_RETURN_ON_FALSE(board && board->pmic, ESP_ERR_INVALID_ARG, TAG, "board not initialized");
    return tg29_set_rail_enabled(board->pmic, TG29_RAIL_BLDO1, enabled);
}
