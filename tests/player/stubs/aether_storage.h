#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *aether_storage_handle_t;
esp_err_t aether_storage_acquire(aether_storage_handle_t, uint32_t);
void aether_storage_release(aether_storage_handle_t);
bool aether_storage_card_inserted(aether_storage_handle_t);
