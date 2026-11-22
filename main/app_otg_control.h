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
    uint8_t cc_detection_raw; /* REG0B from the same power snapshot. */
    uint8_t cc_mode_raw; /* REGE3 from the same power snapshot. */
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
esp_err_t app_otg_set_camera_profile(bool enabled);
esp_err_t app_otg_set_manual_enabled(bool enabled);
/* One accessory lease. Acquire from OFF starts a manual session; acquire on an
 * existing user manual session borrows it. AUTO/teardown are not taken over.
 * Commands are nonblocking; ESP_ERR_NOT_FINISHED means retry after contention.
 * A valid lease is admission only, NOT proof that output_verified is true.
 * User OFF, shutdown and faults invalidate the token. A stale release never
 * affects a newer session; releasing a borrowed session leaves user power ON. */
esp_err_t app_otg_manual_lease_acquire(uint32_t *token);
esp_err_t app_otg_manual_lease_release(uint32_t token);
bool app_otg_manual_lease_valid(uint32_t token);
typedef struct {
    bool valid;
    bool present;
    bool voltage_valid;
    uint16_t voltage_mv;
} app_otg_input_status_t;
/* Worker-only external-input measurement lease; never starts Host or changes
 * the mux/CC/BOOST/TS/charger. NOT_FOUND alone permits trying manual source
 * admission. A failed acquire may return a nonzero cleanup token: retain and
 * release it. All other errors must not fall back to source power. Read is
 * fresh I2C, not a voltage-ready proof; wait for settled consecutive samples.
 * User source OFF leaves this input lease intact. Failed release retains its
 * token; only a successful release permits another source/input session. */
esp_err_t app_otg_external_input_acquire(uint32_t *token);
esp_err_t app_otg_external_input_read(uint32_t token, app_otg_input_status_t *status);
esp_err_t app_otg_external_input_release(uint32_t token);
esp_err_t app_otg_get_status(aether_usb_host_status_t *status);
/* Cache only: no I2C/ADC/USB operations. Live snapshots expire after 2000ms;
 * historical failures remain readable with failure_snapshot=true and age_ms. */
esp_err_t app_otg_get_power_status(app_otg_power_status_t *status);
esp_err_t app_otg_disable_wait(uint32_t timeout_ms);
