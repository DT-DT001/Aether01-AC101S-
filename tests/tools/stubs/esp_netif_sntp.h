#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    const char *servers[1];
} esp_sntp_config_t;

#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) { .servers = { server } }

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
void esp_netif_sntp_deinit(void);
esp_err_t esp_netif_sntp_sync_wait(TickType_t timeout);
