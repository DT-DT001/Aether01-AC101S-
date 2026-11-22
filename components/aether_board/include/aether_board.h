#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "dps368.h"
#include "ac101s_audio.h"
#include "pcf85063.h"
#include "qmi8658a.h"
#include "tg29.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_bus_handle_t system_lp_i2c;
    i2c_master_bus_handle_t audio_i2c;
    i2c_master_bus_handle_t touch_i2c;
    tg29_handle_t pmic;
    bool otg_vbus_adc_trusted; /* Plausible external-input baseline this boot. */
    qmi8658a_handle_t imu;
    dps368_handle_t barometer;
    pcf85063_handle_t rtc;
    ac101s_audio_handle_t audio;
    esp_lcd_panel_io_handle_t display_io;
    esp_lcd_panel_handle_t display;
    esp_lcd_touch_handle_t touch;
    bool imu_ready;
    bool barometer_ready;
    bool rtc_ready;
    bool audio_ready;
    bool display_ready;
    bool touch_ready;
    bool display_sleeping;
    bool display_needs_reinit;
    bool touch_sleeping;
    uint8_t display_brightness;
} aether_board_t;

/**
 * Bring up safe outputs, PMIC power and a blank display.
 *
 * This stage lets the application present its first frame before probing the
 * remaining hardware. Keep the panel off until that frame has been written.
 */
esp_err_t aether_board_init_display(aether_board_t *board);
/** Complete optional peripheral and input setup after the first frame. */
esp_err_t aether_board_init_peripherals(aether_board_t *board);

/** BLDO1 is deliberately off after boot and is switched only for microSD use. */
esp_err_t aether_board_set_sd_power(aether_board_t *board, bool enabled);

/** Enter/leave ordinary CO5300 sleep while retaining CLDO3/AVDD_EN and touch.
 * A failed command marks the display for a full cold-initialization recovery. */
esp_err_t aether_board_set_display_sleep(aether_board_t *board, bool sleeping);

/** Set the panel's internal 0..100 brightness control. */
esp_err_t aether_board_set_display_brightness(aether_board_t *board,
                                              uint8_t percent);

/** Put CST9220 into its command sleep state; normally kept awake for screen wake. */
esp_err_t aether_board_set_touch_sleep(aether_board_t *board, bool sleeping);

#ifdef __cplusplus
}
#endif
