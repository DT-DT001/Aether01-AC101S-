#pragma once

#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CST9220_I2C_ADDRESS 0x5A
#define CST9220_CHIP_ID     0x9220

/**
 * Create a CST9220 handle compatible with esp_lcd_touch and esp_lvgl_port.
 * The touch rail must already be stable before this function is called.
 */
esp_err_t cst9220_new(i2c_master_bus_handle_t bus,
                      const esp_lcd_touch_config_t *config,
                      esp_lcd_touch_handle_t *out_touch);

/** Sequence of the latest well-formed physical report, including release.
 * ACK echoes, markerless polls, and malformed/read-failed reports cannot
 * advance it. Zero means no valid report yet or a non-CST9220 handle.
 * Compare for inequality to detect a new report; the sequence can wrap. */
uint32_t cst9220_report_sequence(esp_lcd_touch_handle_t touch);

#ifdef __cplusplus
}
#endif
