#pragma once

#include "esp_err.h"

esp_err_t esp_netif_tcpip_exec(esp_err_t (*callback)(void *), void *context);
