#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define AETHER_STORAGE_MOUNT_POINT "/sdcard"
typedef void *aether_storage_handle_t;
typedef struct {bool mounted,card_inserted;uint64_t capacity_bytes;esp_err_t last_error;} aether_storage_status_t;
bool aether_storage_is_mounted(aether_storage_handle_t);
esp_err_t aether_storage_get_status(aether_storage_handle_t,aether_storage_status_t *);
#include "aether_storage_probe.h"
esp_err_t aether_storage_get_diagnostics(aether_storage_handle_t,aether_storage_diagnostics_t*);
esp_err_t aether_storage_request_retry(aether_storage_handle_t);
