#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_power.h"
#include "esp_err.h"
#include "gnss_nmea.h"

typedef enum {
    APP_GPS_OFF,
    APP_GPS_POWERING,
    APP_GPS_SEARCHING,
    APP_GPS_FIXED,
    APP_GPS_STOPPING,
    APP_GPS_ERROR,
} app_gps_state_t;

typedef struct {
    bool available, requested_enabled, active;
    bool uart_receiving, pps_seen, supply_valid;
    bool external_power; /* Existing USB input, no OTG/Host/source lease. */
    app_gps_state_t state;
    esp_err_t last_error;
    uint16_t supply_mv;
    uint32_t pps_count, pps_age_ms, session_ms;
    gnss_nmea_snapshot_t navigation;
} app_gps_status_t;

/* Commands only publish intent. UART and power work never run on LVGL. */
esp_err_t app_gps_init(app_power_t *power);
esp_err_t app_gps_set_enabled(bool enabled);
void app_gps_get_status(app_gps_status_t *status);
void app_gps_cancel(void);
esp_err_t app_gps_stop_wait(uint32_t timeout_ms);
