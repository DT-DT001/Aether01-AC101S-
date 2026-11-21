#pragma once
#include "aether_bluetooth.h"
#include "aether_wifi.h"

/* Opt-in one-shot build diagnostic; never pairs a peer or enters credentials.
 * Start owns a PSRAM controller stack until reboot, excluding it from HP budget.
 * All actual radio setup and persistence remain on the services' owner tasks. */
/* keep_enabled retains successful scans for a following media stress stage. */
void app_radio_diagnostic_run(aether_bluetooth_handle_t bluetooth, aether_wifi_handle_t wifi, bool keep_enabled);
esp_err_t app_radio_diagnostic_start(aether_bluetooth_handle_t bluetooth, aether_wifi_handle_t wifi);
