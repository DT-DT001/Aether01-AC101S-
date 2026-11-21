#pragma once
#include <stdbool.h>
#include "esp_err.h"
/* Optional diagnostic build only: one USB-input-only GNSS run (at most300s),
 * then a passive5s OTG cache monitor. AETHER_GPS_DIAGNOSTIC_PASSIVE skips that
 * run entirely: no GPS start/stop or power commands. The monitor never reads
 * live I2C or persists RTC/NVS data. Normal builds never call this. */
esp_err_t app_gps_diagnostic_start(void);
/* False throughout an automatic USB-only probe, then true only after its
 * stop_wait succeeds. Passive-only builds publish true immediately, allowing
 * user-initiated GPS and battery OTG tests without first running a probe. */
bool app_gps_diagnostic_manual_tests_enabled(void);
