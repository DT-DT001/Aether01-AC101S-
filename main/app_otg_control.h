#pragma once
#include "aether_usb_host.h"

/* Retained worker-owned PMIC diagnostics. Live snapshots expire after 2s.
 * A failure_snapshot is explicitly historical and keeps its captured validity
 * beyond that TTL; output_verified is always false. Never interpret an invalid
 * voltage as a measured zero, or raw ADC units as measured millivolts. */
typedef struct {
    bool valid;
    bool vbus_adc_trusted; /* This sample uses measured output verification. */
    bool manual_source_requested;
    bool manual_source_failed; /* Last manual session was stopped after an error. */
    bool failure_snapshot;
    bool diagnostics_valid; /* Same-phase R50/TS/fault snapshot available. */
    bool boost_enabled; /* Last sampled REG19 command bit, not power-good. */
    bool rbfet_enabled; /* Last sampled REG13.bit0 VBUS output gate. */
    bool output_verified; /* Recent measured VBUS in the allowed source range. */
    bool vbus_valid;
    bool ts_valid;
    uint16_t vbus_mv;
    uint16_t vbus_raw; /* 14-bit ADC units; meaningful with valid and C0.bit2. */
    uint16_t ts_mv;
    uint8_t cc_raw;
    uint8_t control_raw;
    uint8_t rbfet_control_raw; /* REG13, preserved alongside the same power snapshot. */
    uint8_t adc_enable_raw; /* REG C0, not a voltage-valid flag. */
    uint8_t ts_config;
    uint8_t work_faults; /* REG08[1:0], latched working-temperature faults. */
    esp_err_t last_error;
    esp_err_t verify_error;
    esp_err_t cleanup_error;
    esp_err_t diagnostic_error;
    uint32_t age_ms; /* UINT32_MAX before the first diagnostic sample. */
} app_otg_power_status_t;

/* Nonblocking GUI commands and cached status only. */
esp_err_t app_otg_set_enabled(bool enabled);
esp_err_t app_otg_set_manual_enabled(bool enabled);
esp_err_t app_otg_get_status(aether_usb_host_status_t *status);
/* Cache only: no I2C/ADC/USB operations. Live snapshots expire after 2000ms;
 * historical failures remain readable with failure_snapshot=true and age_ms. */
esp_err_t app_otg_get_power_status(app_otg_power_status_t *status);
esp_err_t app_otg_disable_wait(uint32_t timeout_ms);
