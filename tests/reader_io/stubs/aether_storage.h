#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *aether_storage_handle_t;
#define AETHER_STORAGE_MOUNT_POINT "/sdcard"
bool aether_storage_card_inserted(aether_storage_handle_t storage);
esp_err_t aether_storage_acquire(aether_storage_handle_t storage, uint32_t timeout);
void aether_storage_release(aether_storage_handle_t storage);
