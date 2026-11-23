#pragma once
#include "../../reader/stubs/esp_err.h"
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
const char *esp_err_to_name(esp_err_t error);
