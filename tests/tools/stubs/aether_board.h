#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_ERR_NOT_FINISHED 0x10c
typedef struct {
    unsigned year, month, day, hour, minute, second, weekday;
    bool oscillator_stopped;
} pcf85063_time_t;
typedef struct { uint16_t dcdc2_mv; } tg29_power_state_t;
typedef struct {
    bool presence_valid,battery_present,vbus_present,charging,charge_done;
    bool charge_state_valid,percentage_valid,voltage_valid;
    uint8_t percentage,charge_state,current_direction;
    uint16_t voltage_mv;
} tg29_battery_state_t;
typedef struct { bool rtc_ready; void *rtc, *pmic; } aether_board_t;
esp_err_t pcf85063_set_time(void *, const pcf85063_time_t *);
esp_err_t pcf85063_get_time(void *, pcf85063_time_t *);
esp_err_t tg29_get_power_state(void *, tg29_power_state_t *);
esp_err_t tg29_get_battery_state(void *, tg29_battery_state_t *);
