#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCF85063_I2C_ADDRESS 0x51

typedef struct pcf85063_dev_t *pcf85063_handle_t;

typedef struct {
    uint16_t year;  /* 2000..2099 */
    uint8_t month;  /* 1..12 */
    uint8_t day;    /* 1..31 */
    uint8_t weekday;/* 0..6 */
    uint8_t hour;   /* 0..23 */
    uint8_t minute; /* 0..59 */
    uint8_t second; /* 0..59 */
    bool oscillator_stopped;
} pcf85063_time_t;

esp_err_t pcf85063_new(i2c_master_bus_handle_t bus, pcf85063_handle_t *out_handle);
esp_err_t pcf85063_del(pcf85063_handle_t handle);
esp_err_t pcf85063_get_time(pcf85063_handle_t handle, pcf85063_time_t *time);
/** Read time using a nonnegative I2C timeout in milliseconds.
 * The RTC layer does not log read failures. */
esp_err_t pcf85063_get_time_with_timeout(pcf85063_handle_t handle, pcf85063_time_t *time,
                                        int timeout_ms);
esp_err_t pcf85063_set_time(pcf85063_handle_t handle, const pcf85063_time_t *time);

#ifdef __cplusplus
}
#endif
