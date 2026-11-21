#pragma once
#include <stdbool.h>
#include "esp_err.h"
#define AETHER_STORAGE_MOUNT_POINT "/sdcard"
typedef void *aether_storage_handle_t;
bool aether_storage_is_mounted(aether_storage_handle_t);
